#include "RIC/qoe_processor.hpp"
#include "log.hpp"
#include <iostream>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <mutex>
#include <net/if.h>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <functional>
#include <net/if.h>
#include <sys/resource.h>

namespace ric {

const std::string MODULE = "QOE_PROCESSOR";

QoEProcessor::QoEProcessor() 
    : obj(nullptr), 
      prog(nullptr),
      link(nullptr),
      prog_fd(-1), 
      ringbuf_fd(-1), 
      running(false), 
      total_frames_completed(0), 
      total_frames_timeout(0), 
      total_packets_received(0), 
      total_packets_expected(0), 
      elapsed_frames_completed(0),
      elapsed_bytes_received(0),
      estimated_frame_interval(DEFAULT_FRAME_DEADLINE),
      last_frame_timestamp_(0),
      frame_completion_callback(nullptr), // Initialize callbacks to nullptr
      new_frame_callback(nullptr)
{
    // Initialize QoE metrics
    current_metrics.avg_frame_rate = 0.0;
    current_metrics.avg_bitrate = 0.0;
    current_metrics.avg_frame_delay = 0.0;
    current_metrics.avg_jitter = 0.0;
    current_metrics.packet_loss_rate = 0.0;
    current_metrics.rlc_acked_bytes = 0;
    current_metrics.rlc_throughput_mbps = 0.0;

    recent_timestamp_ = 0;
    
    stats_start_time = std::chrono::steady_clock::now();
    last_stats_update = stats_start_time;
    last_rlc_update_time_ = stats_start_time;
    last_reported_rlc_bytes_ = 0;
    
    // Initialize frame jitter tracking
    last_frame_arrival_time_ = stats_start_time;
    last_frame_completion_time_ = stats_start_time;
}

QoEProcessor::~QoEProcessor()
{
    /* ──────────────────────── 1.  Stop worker threads ──────────────────────── */
    stop();                              // joins processing_thread

    if (frame_update_thread_running_) {
        frame_update_thread_running_ = false;
        frame_update_cv_.notify_all();
        if (frame_update_thread_.joinable())
            frame_update_thread_.join();
    }

    /* ──────────────────────── 2.  Destroy TC / BPF links  ───────────────────── */
    try {
        /* 2‑A. Destroy the explicit link object (preferred) */
        if (link) {                      // link was returned by bpf_tc_attach()
            bpf_link__destroy(link);
            link = nullptr;
            LOG_MODULE_INFO(MODULE, "Destroyed TC/BPF link object");
        }

        /* 2‑B. Fallback – detach via tc_hook if the link was never saved        */
        if (!interface_name.empty()) {
            int ifindex = if_nametoindex(interface_name.c_str());
            if (ifindex != 0) {
                struct bpf_tc_hook tc_hook = {};
                tc_hook.sz          = sizeof(tc_hook);
                tc_hook.ifindex     = ifindex;
                tc_hook.attach_point= BPF_TC_EGRESS;

                struct bpf_tc_opts opts = {};
                opts.sz     = sizeof(opts);
                opts.prog_fd= 0;               // detach
                (void)bpf_tc_detach(&tc_hook, &opts);
                (void)bpf_tc_hook_destroy(&tc_hook);
            }
        }
    } catch (...) {
        /* don’t throw from a destructor */
    }

    /* ──────────────────────── 3.  Remove auto‑pinned maps  ─────────────────── */
    // tc loader pins maps below this directory; remove the ones we know
    system("rm -f /sys/fs/bpf/tc/globals/packet_ringbuf "
           "/sys/fs/bpf/tc/globals/rtp_filter 2>/dev/null");

    /* ──────────────────────── 4.  Flush & close user‑space resources ───────── */
    csvLogger_.reset();                  // ensures flush

    if (frame_stats_file_.is_open()) {
        frame_stats_file_.close();
        LOG_MODULE_INFO(MODULE, "Closed frame stats file");
    }

    /* ──────────────────────── 5.  Close libbpf object last ─────────────────── */
    if (obj) {
        bpf_object__close(obj);
        obj = nullptr;
        LOG_MODULE_INFO(MODULE, "Closed BPF object");
    }
}


bool QoEProcessor::initialize(const std::string& interface, bool test_mode) {
    interface_name = interface;

    // Determine log directory from QCON_LOG_DIR env var (set by run scripts);
    // fall back to ./logs/ (relative to the ric_main launch CWD).
    const char* env_log_dir = std::getenv("QCON_LOG_DIR");
    if (env_log_dir && env_log_dir[0]) {
        log_dir_ = env_log_dir;
        LOG_MODULE_INFO(MODULE, "Using QCON_LOG_DIR=" << log_dir_);
    } else {
        std::string cmd = "ls -td ./logs/*/ 2>/dev/null | head -1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                log_dir_ = buffer;
                if (!log_dir_.empty() && log_dir_.back() == '\n') log_dir_.pop_back();
                LOG_MODULE_INFO(MODULE, "Fallback log directory: " << log_dir_);
            }
            pclose(pipe);
        }
    }

    if (!log_dir_.empty()) {
        std::string stats_file_path = log_dir_ + "/frame_stats.csv";
        csvLogger_ = std::make_unique<AsyncLogger>(stats_file_path);
        csvLogger_->logLine(
            "timestamp,total_size,received_size,required_size,delivery_time_ms,"
            "arrival_time,completion_time,inter_frame_arrival_diff_ms,"
            "inter_frame_completion_diff_ms,frame_jitter_ms,"
            "ric_frame_construction_time_ms,marker_arrival_time,"
            "ue_frame_construction_time_ms,first_rlc_ack_time,last_rlc_ack_time,"
            "rlc_link_count,rlc_link_ids,rlc_bytes_to_be_arrived");
    }
    
    if (test_mode) {
        LOG_MODULE_INFO(MODULE, "Initializing QoE processor in test mode, skipping BPF setup");
        return true;
    }
    
    try {
        // Set resource limits for BPF
        struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
        if (setrlimit(RLIMIT_MEMLOCK, &rlim)) {
            LOG_MODULE_ERROR(MODULE, "Failed to set resource limits: " << strerror(errno));
            LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
            return true;
        }
        
        // Get interface index
        int ifindex = if_nametoindex(interface_name.c_str());
        if (ifindex == 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to get interface index: " << strerror(errno));
            LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
            return true;
        }
        
        LOG_MODULE_DEBUG(MODULE, "Interface " << interface_name << " has index " << ifindex);
        
        // Load the BPF program from the bin directory
        obj = bpf_object__open_file("bin/tc_rtp_filter.o", NULL);
        if (!obj || libbpf_get_error(obj)) {
            LOG_MODULE_ERROR(MODULE, "Failed to open BPF object file: " << strerror(errno));
            LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
            return true;
        }
        
        // Load BPF program into the kernel
        if (bpf_object__load(obj)) {
            LOG_MODULE_ERROR(MODULE, "Failed to load BPF object: " << strerror(errno));
            bpf_object__close(obj);
            obj = nullptr;
            LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
            return true;
        }
        
        // Set up TC qdisc
        std::string tc_cmd = "tc qdisc add dev " + interface_name + " clsact 2>/dev/null || "
                           "tc qdisc replace dev " + interface_name + " clsact";
        int ret = system(tc_cmd.c_str());
        if (ret != 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to create clsact qdisc: " << strerror(errno));
            bpf_object__close(obj);
            obj = nullptr;
            LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
            return true;
        }
        
        LOG_MODULE_DEBUG(MODULE, "Successfully set up clsact qdisc");
        
                prog      = bpf_object__find_program_by_name(obj, "rtp_filter");
        prog_fd   = bpf_program__fd(prog);
        if (prog_fd < 0) throw std::runtime_error("invalid prog fd");

        bpf_tc_hook tc_hook {};
        tc_hook.sz          = sizeof(tc_hook);
        tc_hook.ifindex     = ifindex;
        tc_hook.attach_point= BPF_TC_EGRESS;
        bpf_tc_hook_create(&tc_hook);         // ignore -EEXIST

        bpf_tc_opts tc_opts {};
        tc_opts.sz      = sizeof(tc_opts);
        tc_opts.prog_fd = prog_fd;
        tc_opts.flags   = BPF_TC_F_REPLACE;   // ★ overwrite any previous program
        if (int err = bpf_tc_attach(&tc_hook, &tc_opts); err) {
            throw std::runtime_error("bpf_tc_attach failed: " + std::to_string(err));
        }

        // Ring buffer map
        bpf_map* map = bpf_object__find_map_by_name(obj, "packet_ringbuf");
        ringbuf_fd   = bpf_map__fd(map);
        if (ringbuf_fd < 0) throw std::runtime_error("ringbuf fd invalid");

        LOG_MODULE_INFO(MODULE, "eBPF TC program successfully attached to " << interface_name);
        return true;
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Exception occurred during eBPF setup: " << e.what());
        
        // Clean up BPF resources
        if (obj) {
            bpf_object__close(obj);
            obj = nullptr;
        }
        
        LOG_MODULE_WARN(MODULE, "Continuing in test mode without BPF");
        return true;
    }
}

bool QoEProcessor::start() {
    if (running.load()) {
        LOG_MODULE_WARN(MODULE, "QoE processor already running");
        return true;
    }
    
    if (ringbuf_fd < 0) {
        LOG_MODULE_ERROR(MODULE, "Ring buffer not initialized");
        return false;
    }
    
    // Start the main processing thread
    running.store(true);
    processing_thread = std::thread(&QoEProcessor::processingThread, this);
    
    // Start the frame update thread
    frame_update_thread_running_ = true;
    frame_update_thread_ = std::thread(&QoEProcessor::frameUpdateThread, this);
    LOG_MODULE_INFO(MODULE, "Frame update notification thread started");
    
    LOG_MODULE_INFO(MODULE, "QoE processor started");
    return true;
}

void QoEProcessor::stop() {
    if (!running.load()) {
        return;
    }
    
    running.store(false);
    
    if (processing_thread.joinable()) {
        processing_thread.join();
    }
    
    // Clean up TC filter using proper TC hook detachment
    if (!interface_name.empty()) {
        try {
            // Get interface index
            int ifindex = if_nametoindex(interface_name.c_str());
            if (ifindex != 0) {
                // Initialize TC hook for cleanup
                struct bpf_tc_hook tc_hook = {};
                tc_hook.sz = sizeof(struct bpf_tc_hook);
                tc_hook.ifindex = ifindex;
                tc_hook.attach_point = static_cast<enum bpf_tc_attach_point>(BPF_TC_EGRESS);
                
                // Detach TC program
                struct bpf_tc_opts opts = {};
                opts.sz = sizeof(struct bpf_tc_opts);
                opts.flags = 0;
                opts.prog_fd = 0; // Setting to 0 detaches the program
                
                int err = bpf_tc_detach(&tc_hook, &opts);
                if (err) {
                    LOG_MODULE_ERROR(MODULE, "Failed to detach TC program: " << err);
                } else {
                    LOG_MODULE_INFO(MODULE, "Successfully detached TC program from interface " << interface_name);
                }
                
                // Destroy the hook
                err = bpf_tc_hook_destroy(&tc_hook);
                if (err) {
                    LOG_MODULE_ERROR(MODULE, "Failed to destroy TC hook: " << err);
                } else {
                    LOG_MODULE_DEBUG(MODULE, "Successfully destroyed TC hook");
                }
            }
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Exception during TC hook cleanup: " << e.what());
        }
        
        // As a fallback, also attempt to remove via tc command
        //std::string tc_cmd = "tc filter del dev " + interface_name + " ingress 2>/dev/null || true";
        //system(tc_cmd.c_str());
        //LOG_MODULE_INFO(MODULE, "TC filter cleanup completed for interface " << interface_name);
    }
    
    LOG_MODULE_INFO(MODULE, "QoE processor stopped");
}

Json::Value QoEProcessor::getQoEMetrics() const {
    
    Json::Value metrics;
    metrics["avg_frame_rate"] = current_metrics.avg_frame_rate;
    metrics["avg_bitrate"] = current_metrics.avg_bitrate;
    metrics["avg_frame_delay"] = current_metrics.avg_frame_delay;
    // Alias: scheduler reads "avg_frame_completion_time" (eq3 prev_frame_completion).
    // Semantically same as avg_frame_delay (= start_time → delivery_time per frame).
    metrics["avg_frame_completion_time"] = current_metrics.avg_frame_delay;
    metrics["avg_jitter"] = current_metrics.avg_jitter;
    metrics["packet_loss"] = current_metrics.packet_loss_rate;  // alias for scheduler logging
    metrics["packet_loss_rate"] = current_metrics.packet_loss_rate;
    
    // RLC metrics
    metrics["rlc_acked_bytes"] = static_cast<Json::UInt64>(current_metrics.rlc_acked_bytes);
    metrics["rlc_throughput_mbps"] = current_metrics.rlc_throughput_mbps;
    
    metrics["total_frames_completed"] = static_cast<Json::UInt64>(total_frames_completed);
    metrics["total_frames_timeout"] = static_cast<Json::UInt64>(total_frames_timeout);
    metrics["total_packets_received"] = static_cast<Json::UInt64>(total_packets_received);
    metrics["total_packets_expected"] = static_cast<Json::UInt64>(total_packets_expected);
    
    return metrics;
}

bool QoEProcessor::updateTxMetrics(uint64_t pdcp_total,
                                   uint64_t acked,
                                   int      link_id) {
    
    // Calculate RLC throughput based on all links
    auto now          = std::chrono::steady_clock::now();
    auto time_diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - last_rlc_update_time_).count();
    if (time_diff_ms == 0) time_diff_ms = 1;          // avoid div‑by‑zero

    {
        if (pdcp_total != last_pdcp_txed_total_[link_id]) {
            //updateFrameTxed(link_id, pdcp_total);
        }
        if (acked > 0) {
            //LOG_MODULE_ERROR (MODULE, "BW: "<< acked*time_diff_ms << " link: " << link_id << " time_diff: "<<time_diff_ms);
            updateFrameProgress(link_id, acked*time_diff_ms);
        }
    }

    last_rlc_update_time_ = now;
    return true;
}

void QoEProcessor::updateFrameTxed(int link_id, uint64_t txed_total)
{
    std::lock_guard<std::mutex> lock(frames_mutex);

    /* ① delta 계산(기존 유지) */
    uint64_t prev       = last_pdcp_txed_total_[link_id];
    uint64_t txed_bytes = (txed_total > prev) ? (txed_total - prev) : 0;
    last_pdcp_txed_total_[link_id] = txed_total;
    if (txed_bytes == 0) return;

    /* ② ── 수정할 부분 시작 ───────────────────────────────────── */
    uint32_t target_ts = 0;

    // arrival 순서대로 돌면서 가장 앞쪽 未완료( all_txed==false ) 프레임 선택
    for (uint32_t ts : frame_arrival_order) {
        auto it = frames.find(ts);
        if (it != frames.end()
            && it->second.status == FrameDeliveryStatus::IN_PROGRESS
            && !it->second.all_txed)           // 아직 PDCP 전부 못 보낸 프레임
        {
            target_ts = ts;                    // ★ anchor 가 우선적으로 잡힘
            break;
        }
    }
    if (target_ts == 0) {                      // 예외적으로 없으면 그대로 종료
        LOG_MODULE_DEBUG(MODULE,
            "No active frame needs PDCP‑TX bytes for link " << link_id);
        return;
    }
    /* ② ── 수정할 부분 끝 ─────────────────────────────────────── */

    /* ③ 선택된 프레임에 target 바이트 추가 (기존 코드 거의 그대로) */
    auto& rlc = frames[target_ts].frame_rlc_info_;
    rlc.rlc_bytes_to_be_arrived[link_id] += txed_bytes;

    if (frames[target_ts].has_marker)          // marker 이미 받았으면
        frames[target_ts].all_txed = true;     // 더 이상 PDCP‑TX 없음

    LOG_MODULE_WARN(MODULE,
        "[FRAME_TXED] frame="   << target_ts
        << " link="             << link_id
        << " +="                << txed_bytes
        << " total_target="     << rlc.rlc_bytes_to_be_arrived[link_id]
        << " all_txed="         << frames[target_ts].all_txed);
}

/**
 * Update frame delivery progress based on RLC acknowledged bytes
 * @param link_id ID of the link that got updated
 * @param acked_bytes New bytes acknowledged since last update
 */
void QoEProcessor::updateFrameProgress(int link_id, uint64_t diff) {


    // Collect frames to complete outside the lock
    std::vector<uint32_t> frames_to_complete;
    
    // CHANGE 1: Don't add credit to remaining_acked_bytes yet, preserve it
    uint64_t remaining_acked_bytes = diff; 
    
    {
        std::lock_guard<std::mutex> lock(frames_mutex);
        // Count frames by status
        int in_progress_count = 0;
        int complete_count = 0;
        int timeout_count = 0;
        
        for (const auto& frame_pair : frames) {
            if (frame_pair.second.status == FrameDeliveryStatus::IN_PROGRESS) in_progress_count++;
            else if (frame_pair.second.status == FrameDeliveryStatus::COMPLETE) complete_count++;
            else if (frame_pair.second.status == FrameDeliveryStatus::TIMEOUT) timeout_count++;
        }
        
        int frames_checked = 0;
        int frames_skipped_status = 0;
        int frames_skipped_no_target = 0;
        int frames_skipped_already_complete = 0;
        
        // Process frames in order, distributing ACK bytes appropriately
        for (uint32_t timestamp : frame_arrival_order) {
            frames_checked++;
            
            // If no more bytes to distribute, we're done
            if (remaining_acked_bytes == 0) {
                LOG_MODULE_WARN(MODULE, "[FRAME_PROCESSING_COMPLETE] link=" << link_id 
                             << " frames_checked=" << frames_checked
                             << " no_more_bytes=true");
                break;
            }
            
            auto it = frames.find(timestamp);
            if (it == frames.end()) {
                LOG_MODULE_WARN(MODULE, "[FRAME_NOT_FOUND] timestamp=" << timestamp);
                continue; 
            }
            
            FrameInfo& frame = it->second;
            
            // Skip completed or timed out frames
            if (frame.status == FrameDeliveryStatus::COMPLETE || frame.status == FrameDeliveryStatus::TIMEOUT) {
                frames_skipped_status++;
                continue;
            }
            
            // Get or create RLC info for this frame
            auto& rlc_info = frames[timestamp].frame_rlc_info_;
            
            // Get required bytes for this link (if any)
            uint64_t target_bytes = 0;
            if (rlc_info.rlc_bytes_to_be_arrived.find(link_id) != rlc_info.rlc_bytes_to_be_arrived.end()) {
                target_bytes = rlc_info.rlc_bytes_to_be_arrived[link_id];
            } else {
                // If this frame doesn't need bytes from this link, skip it
                frames_skipped_no_target++;
                
                LOG_MODULE_WARN(MODULE, "[FRAME_SKIPPED_NO_TARGET] frame=" << timestamp
                             << " link=" << link_id
                             << " no_target_bytes=true");
                continue;
            }
            
            // Initialize acked bytes if needed
            if (rlc_info.rlc_bytes_acked.find(link_id) == rlc_info.rlc_bytes_acked.end()) {
                rlc_info.rlc_bytes_acked[link_id] = 0;
            }
            
            // Calculate how many more bytes this frame needs
            uint64_t already_acked = rlc_info.rlc_bytes_acked[link_id];
            uint64_t remaining_needed = (target_bytes > already_acked) ? (target_bytes - already_acked) : 0;
            
            // Calculate how many bytes to apply to this frame
            uint64_t bytes_to_apply = std::min(remaining_acked_bytes, remaining_needed);
            uint64_t old_acked = rlc_info.rlc_bytes_acked[link_id];
            
            // Update acked bytes for this frame
            rlc_info.rlc_bytes_acked[link_id] += bytes_to_apply;
            
            // Subtract from remaining bytes
            remaining_acked_bytes -= bytes_to_apply;
            
            // Calculate progress percentage
            double progress_pct = static_cast<double>(rlc_info.rlc_bytes_acked[link_id]) / target_bytes * 100.0;
            std::string progress_str = std::to_string(progress_pct) + "%";

            bool all_links_acked = isFullyAcked(rlc_info);   // 아래 헬퍼 함수
            // If all links have acked their required bytes, mark frame for completion
            if (all_links_acked && !rlc_info.rlc_bytes_to_be_arrived.empty() && frame.has_marker) {
                
                // Update frame completion info
                frame.last_rlc_ack_time = std::chrono::steady_clock::now();
                frame.has_rlc_ack_end = true;        
                if (!frame.has_rlc_ack_start) {
                    frame.first_rlc_ack_time = frame.last_rlc_ack_time;
                    frame.has_rlc_ack_start = true;
                } 
                for (const auto& pair : rlc_info.rlc_bytes_to_be_arrived) {
                    frame.received_size += frame.frame_rlc_info_.rlc_bytes_acked[pair.first];
                    frame.required_size += pair.second;
                }

                //frame.has_marker = false;
                
                //frames_to_complete.push_back(timestamp);
                if (timestamp == oldest_inflight_) {
                    frames_to_complete.push_back(timestamp);   // anchor 즉시 완료
                    advanceOldestInflight();                   // 다음 anchor 선정
                    drainReadyFrames(frames_to_complete);      // 연속으로 준비된 프레임 방출
                } else {
                    std::lock_guard<std::mutex> lk(ready_mtx_);
                    ready_frames_.insert(timestamp);           // anchor 뒤에서 대기
                }
            }
        }
        
        // Log summary of frame processing
        LOG_MODULE_INFO(MODULE, "[FRAME_PROCESSING_SUMMARY] link=" << link_id
                      << " frames_checked=" << frames_checked
                      << " skipped_status=" << frames_skipped_status
                      << " skipped_no_target=" << frames_skipped_no_target
                      << " skipped_already_complete=" << frames_skipped_already_complete
                      << " to_complete=" << frames_to_complete.size());
    } // End of frames_mutex lock

    std::sort(frames_to_complete.begin(), frames_to_complete.end());
    
    // Complete frames outside the lock to avoid deadlocks
    for (uint32_t ts : frames_to_complete) {
        LOG_MODULE_WARN(MODULE, "[FRAME_COMPLETING] timestamp=" << ts);
        completeFrame(ts);
    }
    cleanupOldFrames();
    
    /*
    LOG_MODULE_ERROR(MODULE, "[FRAME_PROGRESS_COMPLETE] link=" << link_id
                 << " initial_bytes=" << diff
                 << " remaining=" << remaining_acked_bytes
                 << " frames_waiting=" << frames.size());*/
}


double QoEProcessor::getFrameDeliveryProgress(uint32_t timestamp) const {
    //std::lock_guard<std::mutex> lock(frames_mutex);
    
    auto it = frames.find(timestamp);
    if (it == frames.end()) {
        return 0.0; // Frame not found
    }
    
    return calculateFrameProgress(it->second);
}

int64_t QoEProcessor::getFrameDeadline(uint32_t timestamp) const {
    std::lock_guard<std::mutex> lock(frames_mutex);
    
    auto it = frames.find(timestamp);
    if (it == frames.end()) {
        return -1; // Frame not found
    }
    
    const FrameInfo& frame = it->second;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - frame.start_time);
    
    // Calculate remaining time until deadline
    auto deadline = estimated_frame_interval;
    auto remaining = deadline - elapsed;
    
    return remaining.count();
}

Json::Value QoEProcessor::getJitterStats() const {
    //std::lock_guard<std::mutex> lock(metrics_mutex);
    
    Json::Value jitter_stats;
    jitter_stats["avg_jitter_ms"] = current_metrics.avg_jitter;
    
    Json::Value frame_delays_array(Json::arrayValue);
    for (const auto& delay : frame_delays) {
        frame_delays_array.append(static_cast<Json::UInt64>(delay.count()));
    }
    jitter_stats["frame_delays"] = frame_delays_array;
    
    return jitter_stats;
}

void QoEProcessor::setFrameCompletionCallback(std::function<void(uint32_t, std::chrono::milliseconds)> callback) {
    frame_completion_callback = callback;
}

void QoEProcessor::setNewFrameCallback(std::function<void()> callback) {
    new_frame_callback = callback;
    LOG_MODULE_INFO(MODULE, "New frame callback registered");
}

void QoEProcessor::processingThread() {
    struct ring_buffer* rb = nullptr;

    LOG_MODULE_INFO(MODULE, "Processing thread started");
    
    // If we have a valid ring buffer, set up the callback
    if (ringbuf_fd >= 0) {
        LOG_MODULE_DEBUG(MODULE, "Setting up ring buffer for packet processing");
        // Create properly initialized ring buffer options
        struct ring_buffer_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.sz = sizeof(struct ring_buffer_opts);
        rb = ring_buffer__new(ringbuf_fd, 
            [](void* ctx, void* data, size_t size) -> int {
                QoEProcessor* processor = static_cast<QoEProcessor*>(ctx);
                PacketInfo* info = reinterpret_cast<PacketInfo*>(data);
                
                processor->processRtpPacket(*info);
                return 0;
            }, 
            this, 
            &opts);
        
        if (!rb) {
            LOG_MODULE_ERROR(MODULE, "Failed to create ring buffer, will run in test mode");
        } else {
            LOG_MODULE_INFO(MODULE, "Ring buffer created, processing packets...");
        }
    } else {
        LOG_MODULE_INFO(MODULE, "Running in test mode without BPF, simulating QoE data");
    }
    
    // Process events (from ring buffer in normal mode, or generate simulated ones in test mode)
    while (running.load()) {
        if (rb) {
            // Normal mode - poll the ring buffer
            int err = ring_buffer__poll(rb, 5 /* timeout in ms */);
            LOG_MODULE_DEBUG(MODULE, "Ring buffer poll returned: " << err);
            if (err == -EINTR && !running.load()) break;
        } else {
            // Test mode - generate simulated RTP packets
            // Every 33ms, simulate a 30fps video stream
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            
            static uint32_t timestamp = 0;
            static uint16_t seq_num = 0;

            // Simulate a frame with 5 packets
            bool is_marker = false;
            for (int i = 0; i < 5; i++) {
                is_marker = (i == 4); // Last packet has marker bit
                
                // Add packet to frame - simulated test mode uses default IP/user
                addFramePacket(timestamp, seq_num, 1400, is_marker, 0, "test_user");
                seq_num++;
            }
            
            // Move to next frame
            timestamp += 90000 / 30; // 90kHz clock for 30fps
        }
        
        // Update QoE metrics periodically
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_update >= STATS_UPDATE_INTERVAL) {
            updateQoEMetrics();
            //cleanupOldFrames();
            last_stats_update = now;
        }
    }
    
    if (rb) {
        ring_buffer__free(rb);
    }
    
    LOG_MODULE_ERROR(MODULE, "Processing thread exiting");
}

void QoEProcessor::processRtpPacket(const PacketInfo& info) {
    // Get source IP address as string
    struct in_addr src_addr;
    src_addr.s_addr = info.src_ip;
    std::string src_ip = inet_ntoa(src_addr);
    
    // Get destination IP address as string
    struct in_addr dst_addr;
    dst_addr.s_addr = info.dst_ip;
    std::string dst_ip = inet_ntoa(dst_addr);
    
    // Generate user ID from destination IP (use IP as user ID for simplicity)
    std::string user_id = dst_ip;
    
    LOG_MODULE_DEBUG(MODULE, "RTP packet: ts=" << info.timestamp 
                   << ", seq=" << info.sequence_number 
                   << ", marker=" << (info.marker ? "1" : "0")
                   << ", size=" << info.packet_size
                   << ", src=" << src_ip << ":" << ntohs(info.src_port)
                   << ", dst=" << dst_ip << ":" << ntohs(info.dst_port)
                   << ", user=" << user_id);
    
    // Map the destination IP to user ID
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        ip_to_user_id_[info.dst_ip] = user_id;
    }
    
    // Add packet to frame with user ID
    addFramePacket(info.timestamp, info.sequence_number, info.packet_size, info.marker, info.dst_ip, user_id);
    
    // Count packet
    total_packets_received++;
}

void QoEProcessor::addFramePacket(uint32_t timestamp, uint16_t seq_num, uint32_t size, bool is_marker, 
                                uint32_t dst_ip, const std::string& user_id) {
    bool new_frame = false;
    bool marker_arrived = false;
    {
        std::lock_guard<std::mutex> lock(frames_mutex);

        // Check if we need to set marker for incomplete previous frames
        if (!frame_arrival_order.empty() && timestamp != frame_arrival_order.back()) {
            // This is a new frame different from the most recent one
            uint32_t prev_timestamp = frame_arrival_order.back();
            auto prev_it = frames.find(prev_timestamp);

            if (prev_it != frames.end() && !prev_it->second.has_marker &&
                prev_it->second.status == FrameDeliveryStatus::IN_PROGRESS) {
                // Previous frame doesn't have marker yet, set marker to true
                frames[prev_timestamp].has_marker = true;
                LOG_MODULE_WARN(MODULE, "Force-setting marker for previous frame " << prev_timestamp
                             << " due to new frame " << timestamp << " arrival");
            }
        }

        // Get or create frame
        auto it = frames.find(timestamp);

        
        // Only create a new frame if it doesn't exist AND timestamp is newer than recent_timestamp_
        if (it == frames.end()) {
            if (timestamp > recent_timestamp_) {

                // New frame
                FrameInfo frame;
                frame.timestamp = timestamp;
                frame.start_time = std::chrono::steady_clock::now();
                frame.last_packet_time = frame.start_time;
                frame.marker_arrival_time = std::chrono::steady_clock::time_point(); // Default/unset
                frame.total_size = 0;
                frame.received_size = 0;
                frame.required_size = 0;
                frame.status = FrameDeliveryStatus::IN_PROGRESS;
                frame.has_marker = false;
                
                // Initialize RLC ACK tracking
                frame.has_rlc_ack_start = false;
                frame.has_rlc_ack_end = false;
                
                frame.all_txed = false;
                
                // Store user identification
                frame.dst_ip = dst_ip;
                frame.user_id = user_id;
                
                // Create a new frame
                auto result = frames.emplace(timestamp, frame);
                it = result.first;
                
                // Also add this frame to the user's frames map if we have a user ID
                if (!user_id.empty()) {
                    std::lock_guard<std::mutex> user_lock(users_mutex_);
                    user_frames_[user_id][timestamp] = frame;
                    LOG_MODULE_DEBUG(MODULE, "Added frame " << timestamp << " to user " << user_id << "'s frames");
                }
                
                // Just initialize the RLC info entry - we'll update values later
                frames[timestamp].frame_rlc_info_ = FrameRlcInfo();
                
                frame_arrival_order.push_back(timestamp);
                recent_timestamp_ = timestamp;
                new_frame = true;

                // Update target timestamp for inflight frames when there exists only one frame in the map
                if (oldest_inflight_ == 0) {    
                    oldest_inflight_ = timestamp;
                }
                
                LOG_MODULE_DEBUG(MODULE, "Created new frame with timestamp " << timestamp);
            } else {
                // Frame doesn't exist and timestamp is older or equal to recent_timestamp_
                // Skip this packet as it might be for an old/discarded frame
                LOG_MODULE_WARN(MODULE, "Skipping packet for frame " << timestamp 
                             << " as it's older than recent frame " << recent_timestamp_);
                return;
            }
        }
        
        // Now we can safely use it->second since we've confirmed it is valid
        FrameInfo& frame = it->second;

        // Update frame information
        auto now = std::chrono::steady_clock::now();
        frame.last_packet_time = now;
        
        // Check if this sequence number is already processed
        auto seq_it = std::find(frame.sequence_numbers.begin(), frame.sequence_numbers.end(), seq_num);
        if (seq_it == frame.sequence_numbers.end()) {
            // New packet
            frame.sequence_numbers.push_back(seq_num);
            frame.packets_received.push_back(true);
            frame.total_size += size;
        }
        
        // Update marker bit if present and record marker arrival time
        if (is_marker && !frame.has_marker) {
            frame.has_marker = true;
            frame.marker_arrival_time = now;
            marker_arrived = true;
            LOG_MODULE_DEBUG(MODULE, "Marker arrived for frame " << timestamp);
            
            // Now that we have the complete frame, distribute the total size according to split ratios
            if (!recent_split_ratios_.empty()) {
                // ... rest of code for split ratios ...
                // (This part is unchanged from your original code)
                
                // Clear any previous allocations
                frame.frame_rlc_info_.rlc_bytes_to_be_arrived.clear();
                
                if (recent_split_ratios_.size() == 1) {
                    // Single link case
                    auto link_id = recent_split_ratios_.begin()->first + 1;
                    frame.frame_rlc_info_.rlc_bytes_to_be_arrived[link_id] = frame.total_size;
                    LOG_MODULE_DEBUG(MODULE, "Frame " << timestamp << " total size " << frame.total_size 
                                << " bytes assigned to single link " << link_id);
                } else {
                    // Multilink case - distribute according to ratios
                    uint32_t bytes_assigned = 0;
                    uint32_t total_bytes = frame.total_size;
                    
                    // Sort links by ID for consistency
                    std::vector<std::pair<int, double>> links_with_ratios;
                    for (const auto& [link_id, ratio] : recent_split_ratios_) {
                        links_with_ratios.push_back({link_id, ratio});
                    }
                    std::sort(links_with_ratios.begin(), links_with_ratios.end());
                    
                    // Calculate byte allocation for each link
                    std::string allocation_log = "Frame " + std::to_string(timestamp) + 
                                            " size " + std::to_string(total_bytes) + " bytes distributed: ";
                    
                    // Last link gets any remaining bytes to ensure we assign exactly total_size
                    for (size_t i = 0; i < links_with_ratios.size(); i++) {
                        int link_id = links_with_ratios[i].first;
                        double ratio = links_with_ratios[i].second;
                        
                        uint32_t link_bytes;
                        if (i == links_with_ratios.size() - 1) {
                            // Last link gets remaining bytes
                            link_bytes = total_bytes - bytes_assigned;
                        } else {
                            // Calculate bytes based on ratio
                            link_bytes = static_cast<uint32_t>(total_bytes * ratio);
                            bytes_assigned += link_bytes;
                        }
                        
                        frame.frame_rlc_info_.rlc_bytes_to_be_arrived[link_id] = link_bytes;
                        allocation_log += "link " + std::to_string(link_id) + 
                                        ": " + std::to_string(link_bytes) + " bytes (" + 
                                        std::to_string(ratio * 100) + "%), ";
                    }
                    
                    LOG_MODULE_DEBUG(MODULE, allocation_log);
                }
            } else {
                // No split ratios available, assign everything to link 1
                frame.frame_rlc_info_.rlc_bytes_to_be_arrived[1] = frame.total_size;
                LOG_MODULE_DEBUG(MODULE, "Frame " << timestamp << " total size " << frame.total_size 
                            << " bytes assigned to default link 1 (no split ratios)");
            }

            // Since we have marker, indicate all bytes are to be transmitted
            frame.all_txed = true;
        }
        elapsed_bytes_received += size;
    }

    // Rest of the function (callbacks, etc.) remains the same
    // Trigger new frame callback with delay calculation if this is a new frame
    if (new_frame && new_frame_callback) {
        // frame_rate 기반 타이밍 계산: time + 1/frame_rate - 3ms
        double frame_rate = (current_metrics.avg_frame_rate > 0) ? 
                            current_metrics.avg_frame_rate : 30.0;  // 기본값 30fps
        int frame_interval_ms = static_cast<int>(1000.0 / frame_rate);
        int delay_ms = frame_interval_ms - 3;  // 3ms 빼기
        
        // 타이머 큐를 통해 콜백 스케줄링
        scheduleFrameCallback(timestamp, delay_ms);
        LOG_MODULE_DEBUG(MODULE, "Scheduled new frame callback for frame " << timestamp 
                       << " with delay " << delay_ms << "ms");
    }
    
    // Schedule frame update messages to be sent by the timer thread
    if ((marker_arrived) && zmq_interface_) {
        Json::Value root;
        root["command"] = "frame_update";
        
        // Convert to string
        Json::FastWriter writer;
        std::string params = writer.write(root);
        
        // Send command to CU using ZmqInterface
        bool result = zmq_interface_->sendCuCommand(params);
        if (result) {
            // Success
        } else {
            LOG_MODULE_ERROR(MODULE, "Failed to send frame update to CU");
        }
    }
}

void QoEProcessor::scheduleFrameCallback(uint32_t timestamp, int delay_ms) {
    if (!frame_update_thread_running_) {
        LOG_MODULE_WARN(MODULE, "Cannot schedule frame callback: frame update thread not running");
        return;
    }
    
    // Create the frame update info
    FrameUpdateInfo update_info;
    update_info.timestamp = timestamp;
    update_info.event_type = "frame_callback";  // 새로운 이벤트 타입
    
    // Calculate scheduled time
    auto now = std::chrono::steady_clock::now();
    update_info.scheduled_time = now + std::chrono::milliseconds(delay_ms);
    
    // Add to the queue with mutex protection
    {
        std::lock_guard<std::mutex> lock(frame_update_queue_mutex_);
        frame_update_queue_.push(update_info);
    }
    
    // Notify the frame update thread
    frame_update_cv_.notify_one();
}


// helper: convert steady‑clock TP to system‑clock ms since epoch
static inline int64_t to_ms_epoch(std::chrono::steady_clock::time_point tp)
{
    auto sys_tp = std::chrono::system_clock::now()
                - (std::chrono::steady_clock::now() - tp);

    return std::chrono::duration_cast<std::chrono::milliseconds>(
               sys_tp.time_since_epoch())
           .count();
}

// Schedule a frame update to be sent at a specified time
void QoEProcessor::scheduleFrameUpdate(uint32_t timestamp, bool is_marker, int delay_ms) {
    if (!zmq_interface_) {
        LOG_MODULE_WARN(MODULE, "Cannot schedule frame update: ZMQ interface not available");
        return;
    }
    
    // Create the frame update info
    FrameUpdateInfo update_info;
    update_info.timestamp = timestamp;
    update_info.event_type = is_marker ? "marker" : "new_frame";
    
    // Calculate scheduled time
    auto now = std::chrono::steady_clock::now();
    update_info.scheduled_time = now + std::chrono::milliseconds(delay_ms);
    
    // Add to the queue with mutex protection
    {
        std::lock_guard<std::mutex> lock(frame_update_queue_mutex_);
        frame_update_queue_.push(update_info);
    }
    
    // Notify the frame update thread
    frame_update_cv_.notify_one();
}

// Thread function for sending frame updates at scheduled times
void QoEProcessor::frameUpdateThread() {
    LOG_MODULE_INFO(MODULE, "Frame update thread started");
    
    while (frame_update_thread_running_) {
        FrameUpdateInfo update_info;
        bool has_job = false;
        
        // Wait with timeout or until notified
        {
            std::unique_lock<std::mutex> lock(frame_update_queue_mutex_);
            
            if (frame_update_queue_.empty()) {
                // Wait for notification with timeout
                frame_update_cv_.wait_for(lock, std::chrono::milliseconds(100), 
                    [this] { return !frame_update_thread_running_ || !frame_update_queue_.empty(); });
                
                if (!frame_update_thread_running_) {
                    break;  // Exit the thread if we're shutting down
                }
                
                if (frame_update_queue_.empty()) {
                    continue;  // Nothing to do yet
                }
            }
            
            // Get the next update from the queue
            update_info = frame_update_queue_.front();
            
            // Check if it's time to process this update
            auto now = std::chrono::steady_clock::now();
            if (now >= update_info.scheduled_time) {
                frame_update_queue_.pop();
                has_job = true;
            } else {
                // Not time yet, sleep until the scheduled time or next check
                auto wait_time = std::min(
                    std::chrono::milliseconds(100),  // Max wait time
                    std::chrono::duration_cast<std::chrono::milliseconds>(update_info.scheduled_time - now)
                );
                
                frame_update_cv_.wait_for(lock, wait_time, 
                    [this] { return !frame_update_thread_running_; });
                
                if (!frame_update_thread_running_) {
                    break;  // Exit the thread if we're shutting down
                }
                
                continue;  // Re-check the queue
            }
        }
        
        // Process the update outside the lock
        if (has_job) {
            if (update_info.event_type == "frame_callback") {
                // 새 프레임 콜백 처리
                if (new_frame_callback) {
                    try {
                        new_frame_callback();
                        LOG_MODULE_DEBUG(MODULE, "Executed new frame callback for frame " 
                                       << update_info.timestamp);
                    } catch (const std::exception& e) {
                        LOG_MODULE_ERROR(MODULE, "Error in new frame callback: " << e.what());
                    }
                }
            } else if (update_info.event_type == "marker" || update_info.event_type == "new_frame") {
                // 기존 frame update 처리
                if (zmq_interface_) {
                    // Create JSON parameters for frame update command
                    Json::Value root;
                    root["command"] = "frame_update";
                    root["timestamp"] = Json::Value::UInt64(update_info.timestamp);
                    root["event_type"] = update_info.event_type;
                    
                    // Convert to string
                    Json::FastWriter writer;
                    std::string params = writer.write(root);
                    
                    // Send command to CU using ZmqInterface
                    bool result = zmq_interface_->sendCuCommand(params);
                    if (result) {
                        LOG_MODULE_DEBUG(MODULE, "Sent frame update to CU: timestamp=" 
                                       << update_info.timestamp 
                                       << ", type=" << update_info.event_type);
                    } else {
                        LOG_MODULE_ERROR(MODULE, "Failed to send frame update to CU");
                    }
                }
            }
        }
    }
    
    LOG_MODULE_INFO(MODULE, "Frame update thread exiting");
}

void QoEProcessor::completeFrame(uint32_t timestamp) {
    // Make a copy of data needed outside the lock
    uint32_t seq_numbers_size = 0;
    std::chrono::milliseconds delivery_time;
    bool should_notify = false;

    bool hasMarker  = false;
    bool hasAckSpan = false;
    std::chrono::steady_clock::time_point
        markerTP, firstAckTP, lastAckTP, startTP;
    
    // For frame jitter calculation
    auto now = std::chrono::steady_clock::now();
    auto frame_completion_time = now;
    uint32_t prev_timestamp = 0;
    std::chrono::steady_clock::time_point frame_arrival_time;
    std::chrono::milliseconds inter_frame_jitter{0};
    int64_t inter_frame_arrival_diff_ms = 0;
    int64_t inter_frame_completion_diff_ms = 0;
    uint32_t total_size = 0;
    uint32_t received_size = 0;
    uint32_t required_size = 0;


    std::string rlc_link_ids = "";
    std::string rlc_bytes_to_be_arrived = "";
    int link_count = 0;
    
    std::string user_id;
    {
        std::lock_guard<std::mutex> lock(frames_mutex);
        auto it = frames.find(timestamp);
        if (it == frames.end()) {
            return; // Frame not found
        }
        
        FrameInfo& frame = it->second;
        user_id = frame.user_id; // Save user ID for updating user_frames_ map
        
        // Only process if the frame is not already complete
        if (frame.status != FrameDeliveryStatus::COMPLETE) {
            frame.delivery_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - frame.start_time);
            delivery_time = frame.delivery_time;
            frame_arrival_time = frame.start_time;
            
            frame.status = FrameDeliveryStatus::COMPLETE;
            total_frames_completed++;
            elapsed_frames_completed++;
            
            // Save frame data for logging
            total_size = frame.total_size;
            received_size = frame.received_size;
            required_size = frame.required_size;
            
            // Calculate inter-frame jitter if we have a previous frame
            if (last_frame_timestamp_ != 0 && last_frame_timestamp_ != timestamp) {
                prev_timestamp = last_frame_timestamp_;
                
                // Inter-frame arrival difference (ms)
                inter_frame_arrival_diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame.start_time - last_frame_arrival_time_).count();
                
                // Inter-frame completion difference (ms)
                inter_frame_completion_diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_frame_completion_time_).count();
                
                // Frame jitter is the difference between these two differences
                int64_t jitter_ms = inter_frame_completion_diff_ms - inter_frame_arrival_diff_ms;
                inter_frame_jitter = std::chrono::milliseconds(jitter_ms);
                
                LOG_MODULE_DEBUG(MODULE, "Frame jitter: " << jitter_ms << "ms (completion diff: " 
                              << inter_frame_completion_diff_ms << "ms, arrival diff: " 
                              << inter_frame_arrival_diff_ms << "ms)");
            }
            
            // Update last frame tracking for jitter calculation
            last_frame_timestamp_ = timestamp;
            last_frame_arrival_time_ = frame.start_time;
            last_frame_completion_time_ = now;
            
            // Update total expected packets
            seq_numbers_size = frame.sequence_numbers.size();
            total_packets_expected += seq_numbers_size;
            
            // Update frame delays for jitter calculation
            frame_delays.push_back(frame.delivery_time);
            if (frame_delays.size() > MAX_FRAME_DELAY_HISTORY) {
                frame_delays.pop_front();
            }
            
            // Update estimated frame interval based on recent frame delivery times
            if (frame_delays.size() >= 2) {
                // Use the average of recent frame delivery times
                std::chrono::milliseconds sum{0};
                for (const auto& delay : frame_delays) {
                    sum += delay;
                }
                estimated_frame_interval = std::chrono::milliseconds(sum.count() / frame_delays.size());
                
                // Ensure minimum frame deadline
                if (estimated_frame_interval < DEFAULT_FRAME_DEADLINE) {
                    estimated_frame_interval = DEFAULT_FRAME_DEADLINE;
                }
            }
            
            should_notify = (frame_completion_callback != nullptr);
            
            LOG_MODULE_DEBUG(MODULE, "Frame " << timestamp << " completed in " 
                          << frame.delivery_time.count() << "ms with " 
                          << frame.sequence_numbers.size() << " packets");
        }

        const auto& rlc_info = frame.frame_rlc_info_;
        link_count = rlc_info.rlc_bytes_to_be_arrived.size();
        
        // Create comma-separated lists for link IDs and bytes
        for (const auto& [link_id, bytes] : rlc_info.rlc_bytes_to_be_arrived) {
            if (!rlc_link_ids.empty()) {
                rlc_link_ids += ";";
                rlc_bytes_to_be_arrived += ";";
            }
            rlc_link_ids += std::to_string(link_id);
            rlc_bytes_to_be_arrived += std::to_string(bytes);
        }
        
        hasMarker  = frame.has_marker;                       
        hasAckSpan = frame.has_rlc_ack_start && frame.has_rlc_ack_end;
        markerTP   = frame.marker_arrival_time;
        firstAckTP = frame.first_rlc_ack_time;
        lastAckTP  = frame.last_rlc_ack_time;
        startTP    = frame.start_time;
    }

    total_app_bytes_received += total_size;

    uint64_t rlc_total_now  = 0, pdcp_total_now = 0;
    for (const auto& kv : last_rlc_acked_total_)  rlc_total_now  += kv.second;
    for (const auto& kv : last_pdcp_txed_total_)  pdcp_total_now += kv.second;

    LOG_MODULE_DEBUG(MODULE,
        "[FRAME COMPLETED] RLC_total="   << rlc_total_now
        << " PDCP_total="                << pdcp_total_now
        << " APP_total="                 << total_app_bytes_received
        << " RLC diff="                  << rlc_diff_sum
        << " PDCP diff="                 << pdcp_diff_sum
        << " rlc/app="                   << (double)rlc_total_now / total_app_bytes_received);

    if (csvLogger_) {
        std::ostringstream ss;
        ss << timestamp << ','                             // ① 반드시 timestamp
        << total_size << ','                            // ② 총 프레임 사이즈
        << received_size << ','                         // ③ 수신 바이트
        << required_size << ','                         // ④ 필요 바이트
        << delivery_time.count() << ','                 // ⑤ 전달 지연(ms)
        << to_ms_epoch(frame_arrival_time) << ','       // ⑥ 도착 시각
        << to_ms_epoch(frame_completion_time) << ','    // ⑦ 완료 시각
        << inter_frame_arrival_diff_ms << ','           // ⑧ 이전 프레임과 도착 간격
        << inter_frame_completion_diff_ms << ','        // ⑨ 완료 간격
        << inter_frame_jitter.count() << ','            // ⑩ 지터
        << (hasMarker ?
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    markerTP - startTP).count() : 0) << ','
        << (hasMarker ? to_ms_epoch(markerTP) : 0) << ','
        << (hasAckSpan ?
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    lastAckTP - firstAckTP).count() : 0) << ','
        << (hasAckSpan ? to_ms_epoch(firstAckTP) : 0) << ','
        << (hasAckSpan ? to_ms_epoch(lastAckTP)  : 0) << ','
        << link_count << ','                            // 링크 개수
        << rlc_link_ids << ','                         // 링크 ID 리스트 (세미콜론 구분)
        << rlc_bytes_to_be_arrived;                    // 링크별 필요 바이트 (세미콜론 구분)

        csvLogger_->logLine(ss.str());          // ***비차단 enqueue***
    }

    
    // Update the user frames map for this user
    if (!user_id.empty()) {
        std::lock_guard<std::mutex> user_lock(users_mutex_);
        auto user_it = user_frames_.find(user_id);
        if (user_it != user_frames_.end()) {
            auto frame_it = user_it->second.find(timestamp);
            if (frame_it != user_it->second.end()) {
                // Update the frame status in the user's frame map
                frame_it->second.status = FrameDeliveryStatus::COMPLETE;
                frame_it->second.delivery_time = delivery_time;
                LOG_MODULE_DEBUG(MODULE, "Updated completed frame " << timestamp << " in user " << user_id << "'s frames");
            }
        }
    }
    
    // Notify frame completion via callback outside of lock
    if (should_notify && frame_completion_callback) {
        try {
            frame_completion_callback(timestamp, delivery_time);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Error in frame completion callback: " << e.what());
        }
    }
}

void QoEProcessor::updateQoEMetrics() {
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - stats_last_update_time).count();
    double elapsed_sec = elapsed_ms / 1000.0;

    // Calculate frame rate
    current_metrics.avg_frame_rate = static_cast<double>(elapsed_frames_completed) / elapsed_sec;
    
    current_metrics.avg_bitrate = (elapsed_bytes_received  * 8.0) / (elapsed_sec * 1000.0); // kbps
    
    // Calculate average frame delay
    if (!frame_delays.empty()) {
        std::chrono::milliseconds sum{0};
        for (const auto& delay : frame_delays) {
            sum += delay;
        }
        current_metrics.avg_frame_delay = static_cast<double>(sum.count()) / frame_delays.size();
    }
    
    // Calculate jitter
    current_metrics.avg_jitter = static_cast<double>(calculateJitter().count());
    
    // Calculate packet loss rate
    if (total_packets_expected > 0) {
        double loss_rate = 1.0 - (static_cast<double>(total_packets_received) / total_packets_expected);
        current_metrics.packet_loss_rate = loss_rate;
    } else {
        current_metrics.packet_loss_rate = 0.0;
    }

    stats_last_update_time = std::chrono::steady_clock::now();
    elapsed_bytes_received = 0;
    elapsed_frames_completed = 0;
    
    LOG_MODULE_DEBUG(MODULE, "QoE metrics updated: fps=" << current_metrics.avg_frame_rate
                  << ", bitrate=" << current_metrics.avg_bitrate << "kbps"
                  << ", rlc tp=" << current_metrics.rlc_throughput_mbps << "mbps"
                  << ", delay=" << current_metrics.avg_frame_delay << "ms"
                  << ", jitter=" << current_metrics.avg_jitter << "ms"
                  << ", loss=" << (current_metrics.packet_loss_rate * 100.0) << "%"
                  << ", rlc acked=" << current_metrics.rlc_acked_bytes);
}

void QoEProcessor::cleanupOldFrames()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> timed_out;      // (A) timeout 시킬 프레임
    std::vector<uint32_t> to_erase;       // (B) map 에서 지울 프레임


    std::lock_guard<std::mutex> lock(frames_mutex);

    for (auto & [ts, f] : frames) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - f.last_packet_time);
        if (f.status == FrameDeliveryStatus::COMPLETE ||
            f.status == FrameDeliveryStatus::TIMEOUT)
        {
            to_erase.push_back(ts);
        }
        else if (age >= std::chrono::milliseconds{500})      // 1 s 무응답 == timeout
        {
            timed_out.push_back(ts);                  // (A)
        }
    }

    /* ── ① anchor(및 기타) 프레임 timeout 처리 ─────────────────── */
    for (auto ts : timed_out) {
        timeoutFrame(ts);             // status = TIMEOUT, 카운터 증가
        to_erase.push_back(ts);       // map 에서도 제거
    }

    /* ── ② map·arrival_order 정리 ──────────────────────────────── */
    for (auto ts : to_erase) {
        frames.erase(ts);
        if (auto it = std::find(frame_arrival_order.begin(),
                                frame_arrival_order.end(), ts);
            it != frame_arrival_order.end())
        {
            frame_arrival_order.erase(it);
        }
    }

    /* ── ③ 새 anchor 선정 + ready 큐 방출 ───────────────────────── */
    if (!to_erase.empty()) {
        advanceOldestInflight();          // 새 anchor
        std::vector<uint32_t> releasable;
        drainReadyFrames(releasable);     // anchor 뒤에 줄 서 있던 프레임들
        for (auto ts : releasable) completeFrame(ts);
    }
}

double QoEProcessor::calculateFrameProgress(const FrameInfo& frame) const {
    if (frame.status == FrameDeliveryStatus::COMPLETE) {
        return 1.0;
    }
    
    if (frame.status == FrameDeliveryStatus::TIMEOUT) {
        return 0.0;
    }
    
    // For in-progress frames, calculate based on received packets
    if (frame.total_size > 0) {
        // Calculate progress from RTP packets received
        double rtp_progress = static_cast<double>(frame.received_size) / frame.total_size;
        
        // Check if we have RLC progress information for this frame
        auto rlc_it = frames.find(frame.timestamp);
        if (rlc_it != frames.end()) {
            // If we have updated this frame via RLC ACKs, use that progress value
            // since it's more accurate at the network layer
            return rtp_progress;
        }
        
        return rtp_progress;
    }
    
    return 0.0;
}

void QoEProcessor::timeoutFrame(uint32_t timestamp) {
    uint32_t seq_numbers_size = 0;
    int64_t timeout_duration_ms = 0;
    std::string user_id;
    
    {
        auto it = frames.find(timestamp);
        if (it == frames.end()) {
            return; // Frame not found
        }
        
        FrameInfo& frame = it->second;
        user_id = frame.user_id; // Save user ID for updating user_frames_ map
        
        // Only process if the frame is still in progress
        if (frame.status == FrameDeliveryStatus::IN_PROGRESS) {
            auto now = std::chrono::steady_clock::now();
            timeout_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - frame.start_time).count();
            
            frame.status = FrameDeliveryStatus::TIMEOUT;
            total_frames_timeout++;
            
            // Update total expected packets based on received packets
            seq_numbers_size = frame.sequence_numbers.size();
            total_packets_expected += seq_numbers_size;
            
            // Prepare RLC information strings
            std::ostringstream rlc_target_info;
            std::ostringstream rlc_acked_info;
            
            for (const auto& [link_id, target_bytes] : frame.frame_rlc_info_.rlc_bytes_to_be_arrived) {
                rlc_target_info << "link" << link_id << ":" << target_bytes << " ";
                
                auto it_acked = frame.frame_rlc_info_.rlc_bytes_acked.find(link_id);
                uint64_t acked_bytes = (it_acked != frame.frame_rlc_info_.rlc_bytes_acked.end()) ? 
                                      it_acked->second : 0;
                
                double percent = (target_bytes > 0) ? 
                                (static_cast<double>(acked_bytes) / target_bytes * 100.0) : 0.0;
                
                rlc_acked_info << "link" << link_id << ":" << acked_bytes 
                              << " (" << std::fixed << std::setprecision(1) << percent << "%) ";
            }
            
            // Calculate and log elapsed times
            std::string marker_time_str = "N/A";
            if (frame.has_marker) {
                auto marker_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame.marker_arrival_time - frame.start_time).count();
                marker_time_str = std::to_string(marker_elapsed) + "ms";
            }
            
            std::string rlc_time_str = "N/A";
            if (frame.has_rlc_ack_start) {
                auto rlc_start_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame.first_rlc_ack_time - frame.start_time).count();
                rlc_time_str = std::to_string(rlc_start_elapsed) + "ms";
            }
            
            // Use a single LOG_MODULE_ERROR call with a properly formatted message
            LOG_MODULE_DEBUG(MODULE, 
                "Frame TIMEOUT: timestamp=" << timestamp 
                << " duration=" << timeout_duration_ms << "ms" 
                << " packets=" << seq_numbers_size 
                << " total_size=" << frame.total_size << " bytes" 
                << " has_marker=" << (frame.has_marker ? "yes" : "no") 
                << " marker_time=" << marker_time_str 
                << " rlc_first_ack=" << rlc_time_str 
                << " all_txed=" << (frame.all_txed ? "yes" : "no") 
                << " RLC_targets=" << rlc_target_info.str() 
                << " RLC_acked=" << rlc_acked_info.str() 
                << " InFlight: oldest=" << oldest_inflight_ 
                << " ready_count=" << ready_frames_.size());
        }
    }
    
    // Update the user frames map for this user
    if (!user_id.empty()) {
        std::lock_guard<std::mutex> user_lock(users_mutex_);
        auto user_it = user_frames_.find(user_id);
        if (user_it != user_frames_.end()) {
            auto frame_it = user_it->second.find(timestamp);
            if (frame_it != user_it->second.end()) {
                // Update the frame status in the user's frame map
                frame_it->second.status = FrameDeliveryStatus::TIMEOUT;
                LOG_MODULE_DEBUG(MODULE, "Updated timed out frame " << timestamp << " in user " << user_id << "'s frames");
            }
        }
    }
}

std::unordered_map<uint32_t, FrameInfo> QoEProcessor::getActiveFrames() const
{
    std::unordered_map<uint32_t, FrameInfo> active_frames;

    // Scan the entire frame table; we no longer depend on frame_arrival_order
    for (const auto& [ts, info] : frames) {
        if (info.status == FrameDeliveryStatus::IN_PROGRESS) {
            active_frames.emplace(ts, info);   // copy the entry
        }
    }

    // Debug logging
    if (!active_frames.empty()) {
        std::string frames_str = "Active frames: ";
        for (const auto& [ts, _] : active_frames) {
            frames_str += std::to_string(ts) + ' ';
        }
        LOG_MODULE_DEBUG(MODULE, frames_str);
    } else {
        LOG_MODULE_DEBUG(MODULE, "No active frames found");
    }

    return active_frames;
}

std::unordered_map<uint32_t, FrameInfo> QoEProcessor::getActiveFramesForUser(const std::string& user_id) const
{
    std::unordered_map<uint32_t, FrameInfo> active_frames;

    // First, get user frames from the user frames map
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        auto it = user_frames_.find(user_id);
        if (it != user_frames_.end()) {
            // Copy all in-progress frames for this user
            for (const auto& [ts, info] : it->second) {
                if (info.status == FrameDeliveryStatus::IN_PROGRESS) {
                    active_frames.emplace(ts, info);
                }
            }
        }
    }

    // Debug logging
    if (!active_frames.empty()) {
        std::string frames_str = "Active frames for user " + user_id + ": ";
        for (const auto& [ts, _] : active_frames) {
            frames_str += std::to_string(ts) + ' ';
        }
        LOG_MODULE_DEBUG(MODULE, frames_str);
    } else {
        LOG_MODULE_DEBUG(MODULE, "No active frames found for user " + user_id);
    }

    return active_frames;
}

std::vector<std::string> QoEProcessor::getActiveUsers() const
{
    std::vector<std::string> users;
    
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (const auto& [user_id, frames] : user_frames_) {
            // Only include users with active frames
            bool has_active_frame = false;
            for (const auto& [ts, info] : frames) {
                if (info.status == FrameDeliveryStatus::IN_PROGRESS) {
                    has_active_frame = true;
                    break;
                }
            }
            
            if (has_active_frame) {
                users.push_back(user_id);
            }
        }
    }
    
    LOG_MODULE_DEBUG(MODULE, "Found " << users.size() << " active users");
    return users;
}

void QoEProcessor::saveSchedulingInfo (std::map<int, double> split_ratios) {
    recent_split_ratios_ = split_ratios;
}

std::chrono::milliseconds QoEProcessor::calculateJitter() const {
    if (frame_delays.size() < 2) {
        return std::chrono::milliseconds(0);
    }
    
    // Calculate mean absolute deviation of frame delays
    std::chrono::milliseconds sum{0};
    for (const auto& delay : frame_delays) {
        sum += delay;
    }
    
    std::chrono::milliseconds mean(sum.count() / frame_delays.size());
    
    int64_t deviation_sum = 0;
    for (const auto& delay : frame_delays) {
        int64_t dev = std::abs(delay.count() - mean.count());
        deviation_sum += dev;
    }
    
    return std::chrono::milliseconds(deviation_sum / frame_delays.size());
}

void QoEProcessor::advanceOldestInflight()
{
    for (uint32_t ts : frame_arrival_order) {
        auto it = frames.find(ts);
        if (it != frames.end() &&
            it->second.status == FrameDeliveryStatus::IN_PROGRESS)
        {
            oldest_inflight_ = ts;
            return;
        }
    }
    oldest_inflight_ = 0;   // 미완료 없음
}

void QoEProcessor::drainReadyFrames(std::vector<uint32_t>& out)
{
    std::lock_guard<std::mutex> lk(ready_mtx_);
    while (!ready_frames_.empty() &&
           *ready_frames_.begin() == oldest_inflight_)
    {
        out.push_back(oldest_inflight_);
        ready_frames_.erase(ready_frames_.begin());
        advanceOldestInflight();
    }
}

bool QoEProcessor::isFullyAcked(const FrameRlcInfo& rlc) const
{
    for (auto& [lid, need] : rlc.rlc_bytes_to_be_arrived) {
        if (need == 0) continue;
        auto it = rlc.rlc_bytes_acked.find(lid);
        if (it == rlc.rlc_bytes_acked.end() || it->second < need*0.95)
            return false;
    }
    return !rlc.rlc_bytes_to_be_arrived.empty();
}

double QoEProcessor::getRecentFrameSizeBytes(uint32_t window_ms) const {
    const auto cutoff = std::chrono::steady_clock::now()
                      - std::chrono::milliseconds(window_ms);
    std::lock_guard<std::mutex> lock(frames_mutex);
    uint64_t total = 0;
    uint32_t count = 0;
    for (const auto& [ts, frame] : frames) {
        if (frame.start_time >= cutoff && frame.total_size > 0) {
            total += frame.total_size;
            ++count;
        }
    }
    return count > 0 ? static_cast<double>(total) / count : 0.0;
}

double QoEProcessor::getRecentFrameRateFps(uint32_t window_ms) const {
    const auto cutoff = std::chrono::steady_clock::now()
                      - std::chrono::milliseconds(window_ms);
    std::lock_guard<std::mutex> lock(frames_mutex);
    uint32_t count = 0;
    for (const auto& [ts, frame] : frames) {
        if (frame.start_time >= cutoff) ++count;
    }
    return static_cast<double>(count) * 1000.0 / static_cast<double>(window_ms);
}

double QoEProcessor::getLastCompletedFrameDeliveryMs() const {
    // Use the frame_delays deque (push_back on completion, max history capped).
    // The tail is the most recent completed frame's delivery_time. This avoids
    // the cleanupOldFrames race where COMPLETE frames get evicted before the
    // scheduler reads them.
    std::lock_guard<std::mutex> lock(frames_mutex);
    if (frame_delays.empty()) return 0.0;
    return static_cast<double>(frame_delays.back().count());
}

double QoEProcessor::getRecentMaxFrameJitterMs(uint32_t window_n) const {
    // Per paper §5.3: J_on = max over recent N frames of
    // |inter_completion_diff - inter_arrival_diff|. Since inter_arrival is
    // ~constant (frame period) at a fixed framerate, this reduces to
    // |delivery_time(i) - delivery_time(i-1)| over consecutive frames.
    std::lock_guard<std::mutex> lock(frames_mutex);
    if (frame_delays.size() < 2) return 0.0;
    size_t n = frame_delays.size();
    size_t start = (n > window_n) ? n - window_n : 0;
    double max_jit = 0.0;
    for (size_t i = start + 1; i < n; ++i) {
        int64_t d_prev = frame_delays[i-1].count();
        int64_t d_cur  = frame_delays[i].count();
        double jit = static_cast<double>(std::abs(d_cur - d_prev));
        if (jit > max_jit) max_jit = jit;
    }
    return max_jit;
}

uint32_t QoEProcessor::getNewestFrameTs() const {
    std::lock_guard<std::mutex> lock(frames_mutex);
    if (frame_arrival_order.empty()) return 0;
    return frame_arrival_order.back();
}

std::vector<QoEProcessor::AtRiskFrame>
QoEProcessor::sweepInProgressFrames(uint32_t newest_ts, int64_t deadline_ms) {
    std::vector<AtRiskFrame> out;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(frames_mutex);
    for (auto& [ts, fr] : frames) {
        if (fr.status != FrameDeliveryStatus::IN_PROGRESS) continue;
        if (ts == newest_ts) continue;

        // Per-link ACKed sum + primary link (largest target)
        uint64_t acked = 0;
        int      primary_link  = 0;
        uint64_t primary_target = 0;
        for (const auto& [link_id, target] : fr.frame_rlc_info_.rlc_bytes_to_be_arrived) {
            auto it = fr.frame_rlc_info_.rlc_bytes_acked.find(link_id);
            if (it != fr.frame_rlc_info_.rlc_bytes_acked.end()) acked += it->second;
            if (target > primary_target) {
                primary_target = target;
                primary_link   = link_id - 1;
            }
        }
        uint64_t unacked = (fr.total_size > acked) ? (fr.total_size - acked) : 0;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - fr.start_time);

        if (unacked == 0) {
            // ACK pipeline finished but marker never observed (or per-link
            // race). Promote to COMPLETE so T_{m-1} sees the real number.
            fr.status         = FrameDeliveryStatus::COMPLETE;
            fr.delivery_time  = elapsed;
            fr.last_packet_time = now;
            fr.has_marker     = true;
            frame_delays.push_back(elapsed);
            if (frame_delays.size() > 1000) frame_delays.pop_front();
            continue;
        }

        // Hand candidate to caller. Caller checks BW + slack and decides.
        AtRiskFrame f;
        f.rtp_ts             = ts;
        f.unacked_bytes      = unacked;
        f.primary_link_id    = primary_link;
        f.elapsed_ms         = elapsed.count();
        f.already_reinjected = fr.reinjected_to_alt;
        out.push_back(f);

        // Past-deadline → cleanup. Caller will still see the AtRiskFrame in
        // `out` so it can fire a final reinject if not done already.
        if (elapsed.count() > deadline_ms) {
            fr.status         = FrameDeliveryStatus::TIMEOUT;
            fr.delivery_time  = elapsed;
            fr.last_packet_time = now;
            frame_delays.push_back(elapsed);
            if (frame_delays.size() > 1000) frame_delays.pop_front();
        }
    }
    return out;
}

void QoEProcessor::markFrameReinjected(uint32_t rtp_ts) {
    std::lock_guard<std::mutex> lock(frames_mutex);
    auto it = frames.find(rtp_ts);
    if (it != frames.end()) it->second.reinjected_to_alt = true;
}

std::vector<QoEProcessor::InflightFrameSnapshot>
QoEProcessor::getInflightFrameSnapshots() const {
    std::vector<InflightFrameSnapshot> out;
    std::lock_guard<std::mutex> lock(frames_mutex);
    for (const auto& [ts, frame] : frames) {
        if (frame.status != FrameDeliveryStatus::IN_PROGRESS) continue;
        if (frame.total_size == 0) continue;
        InflightFrameSnapshot s;
        s.rtp_ts = ts;
        s.start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame.start_time.time_since_epoch()).count();
        s.total_bytes = frame.total_size;

        // Sum acked bytes across all links from RLC info
        uint64_t acked = 0;
        int primary_link = 0;
        uint64_t primary_target = 0;
        for (const auto& [link_id, target] : frame.frame_rlc_info_.rlc_bytes_to_be_arrived) {
            auto it = frame.frame_rlc_info_.rlc_bytes_acked.find(link_id);
            if (it != frame.frame_rlc_info_.rlc_bytes_acked.end()) {
                acked += it->second;
            }
            if (target > primary_target) {
                primary_target = target;
                primary_link = link_id;
            }
        }
        s.acked_bytes = acked;
        s.primary_link_id = primary_link;
        out.push_back(s);
    }
    return out;
}

} // namespace ric