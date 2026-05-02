#include "pdcp_sender.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <chrono>
#include <json-c/json.h>
#include "pdcp_common.hpp"

PdcpSender::PdcpSender(const PdcpConfig& cfg)
    : PdcpContext(PdcpRole::SENDER),
      nextSequence(0),
      currentEpoch(0),
      async_logger(cfg.common.logFoldername + "/pdcp_sender.csv"),
      ric_interface_(nullptr),
      ric_thread_running_(false),
      path_bytes_transmitted(cfg.common.path_num),  // Initialize with zeros
      active_path(0),
      kpm_report_interval_ms(cfg.ric.kpm_report_interval_ms ? cfg.ric.kpm_report_interval_ms : 1000),
      kpm_thread_running_(false),
      last_kpm_report_time_(std::chrono::steady_clock::now()),
      duplication_enabled_(false),
      qos_profiler_(std::make_unique<QosProfiler>(QosProfiler::ClassificationMode::SEQUENCE_MOD, 4))
{
    total_link_num = cfg.common.path_num;

    // FORCE_ACTIVE_PATH env: bypass RIC scheduler and pin active_path at boot
    // (used for iperf diagnostics where there's no frame-event traffic to drive
    // the scheduler). 0 = link 0, 1 = link 1, etc.
    if (const char* e = std::getenv("FORCE_ACTIVE_PATH")) {
        try {
            int p = std::stoi(e);
            if (p >= 0 && p < (int)total_link_num) {
                active_path.store(static_cast<uint8_t>(p), std::memory_order_relaxed);
                LOG_MODULE_ERROR(MODULE_SENDER, "[PDCP Sender] FORCE_ACTIVE_PATH="
                                << p << " (initial active_path overridden)");
            } else {
                LOG_MODULE_ERROR(MODULE_SENDER, "[PDCP Sender] FORCE_ACTIVE_PATH=" << p
                                << " out of range, ignored");
            }
        } catch (...) {
            LOG_MODULE_ERROR(MODULE_SENDER, "[PDCP Sender] FORCE_ACTIVE_PATH parse failed");
        }
    } else {
        LOG_MODULE_ERROR(MODULE_SENDER, "[PDCP Sender] FORCE_ACTIVE_PATH not set in env");
    }

    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Initialized "<< static_cast<int>(total_link_num) << " kpm report interval: " << kpm_report_interval_ms.load());
    // Initialize path statistics
    for (int i = 0; i < total_link_num; i++) {
        path_bytes_transmitted[i].store(0, std::memory_order_relaxed);
    }

    // Write CSV header
    async_logger.logLine("pdcp_seq,tcp_seq,timestamp,pkt_len");
    
    // Initialize RIC interface if configured
    if (cfg.ric.ric_enabled) {
        try {
            // Create RIC interface wrapper with minimal parameters
            ric_interface_ = std::make_unique<ric::RicInterfaceWrapper>(
                RIC_COMPONENT_CU,  // Component type as string
                "pdcp_sender",  // Component ID
                cfg.ric.localKpmPort,  // Local KPM port
                cfg.ric.localRcPort,   // Local RC port
                cfg.ric.ipAddress,     // RIC IP
                cfg.ric.kpmPort,       // RIC KPM port
                cfg.ric.rcPort         // RIC RC port
            );
            
            // Initialize and start the interface
            if (!ric_interface_->initInNamespace("node_sender")) {
                LOG_MODULE_ERROR(MODULE_SENDER,"Failed to initialize RIC inside node_sender namespace");
            } else {
                LOG_MODULE_INFO(MODULE_SENDER,"RIC interface is up and running inside node_sender!");

                // Register RC command handlers
                registerRicCommandHandlers();
                
                // Start the KPM metrics reporting thread
                kpm_thread_running_ = true;
                kpm_metrics_thread_ = std::thread(&PdcpSender::kpmMetricsThread, this);
                LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] KPM metrics thread started");

                // Start the RIC hello thread
                //ric_thread_running_ = true;
                //ric_hello_thread_ = std::thread(&PdcpSender::ricHelloThread, this);
                //LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] RIC hello thread started");
            }
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Exception in RIC setup: " << e.what());
        }
    } else {
        LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] RIC interface not configured, skipping initialization");
    }
}

PdcpSender::~PdcpSender() {
    // Stop RIC hello thread if running
    if (ric_thread_running_) {
        ric_thread_running_ = false;
        if (ric_hello_thread_.joinable()) {
            ric_hello_thread_.join();
        }
    }
    
    // Stop KPM metrics thread if running
    if (kpm_thread_running_) {
        kpm_thread_running_ = false;
        if (kpm_metrics_thread_.joinable()) {
            kpm_metrics_thread_.join();
        }
    }
    
    // Stop RIC interface (wrapper handles cleanup)
    if (ric_interface_) {
        ric_interface_->stop();
        ric_interface_.reset();
    }
    
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Cleaned up, final sequence: " << nextSequence.load());
}

size_t PdcpSender::processPacket(unsigned char* packet, size_t len)
{
    size_t pdcpLen    = sizeof(pdcp_hdr);
    size_t totalLen   = pdcpLen + len;
    if (totalLen > PDCP_MAX_PACKET_SIZE) {
        LOG_MODULE_WARN(MODULE_SENDER,"[PDCP Sender] Packet too large after PDCP encaps. Dropping.");
        return 0;
    }

    // Shift the original IP data to make room
    std::memmove(packet + pdcpLen, packet, len);

    // 4) Fill in PDCP header
    pdcp_hdr* pHdr = reinterpret_cast<pdcp_hdr*>(packet);
    std::memset(pHdr, 0, sizeof(pdcp_hdr));

    // --- Retrieve and increment the PDCP sequence ---
    uint32_t seq = nextSequence.fetch_add(1, std::memory_order_relaxed);
    if (seq > PDCP_SEQ_LIMIT) {
        // Wrap
        nextSequence.store(0, std::memory_order_relaxed);
        currentEpoch.fetch_add(1, std::memory_order_relaxed);
        seq = 0;
        LOG_MODULE_WARN(MODULE_SENDER,"[PDCP Sender] Sequence number wrapped, epoch incremented "<< currentEpoch.load());
    }
    uint32_t epoch = currentEpoch.load(std::memory_order_relaxed);

    // Use QoS profiler to determine target queue ID (need sequence number first)
    uint8_t queue_id = 0;
    // QCON_FORCE_QUEUE_0=1 forces every NORMAL packet to queue 0. Reinject
    // burst still overrides pad to its own queue below. Used to make priority-
    // queue ablation comparisons clean: without this, SEQUENCE_MOD distributes
    // normal RTP across queues 0-3, so queue-3 priority effect is muddied.
    static const bool force_q0 = []{
        const char* e = std::getenv("QCON_FORCE_QUEUE_0");
        return (e && std::string(e) == "1");
    }();
    if (qos_profiler_ && !force_q0) {
        // Temporarily set sequence number for classification
        pHdr->sequence_number = htonl(seq);
        queue_id = qos_profiler_->classifyPacket(packet, totalLen);
        // Ensure queue_id is within valid range (0-3 for 4 queues)
        if (queue_id >= 4) {
            queue_id = queue_id % 4;
        }
    }

    pHdr->epoch           = htonl(epoch);
    pHdr->flags           = 0;
    pHdr->pad             = queue_id;  // Store queue ID in pad field
    pHdr->reserved        = 0;
    pHdr->sequence_number = htonl(seq);

    // 5) Insert "send time" in ms
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    uint64_t beTime = htobe64(static_cast<uint64_t>(now_ms));
    std::memcpy(&pHdr->send_timestamp, &beTime, sizeof(uint64_t));

    LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] PDCP epoch=" << epoch
              << " seq=" << seq
              << " totalLen=" << totalLen);

    // Multilink mode (set by handleSetPath when is_multilink=true): weighted
    // random per-packet pick using the cumulative ratio table. Otherwise honor
    // single active_path. Either way fall back to 0 if unset/out-of-range.
    // DIAG: HARDCODE_PATH env forces every packet to one link (bypassing
    // active_path/multilink entirely) — used to confirm pipeline can route
    // to link 1 at all, independent of the active_path-reset bug.
    static const int hardcode_path = []{
        const char* e = std::getenv("HARDCODE_PATH");
        return e ? std::atoi(e) : -1;
    }();
    uint8_t target_path;
    if (hardcode_path >= 0 && hardcode_path < (int)total_link_num) {
        target_path = static_cast<uint8_t>(hardcode_path);
    } else if (multilink_active_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(multilink_state_mutex_);
        if (!multilink_paths_.empty() && multilink_cum_ratios_.size() == multilink_paths_.size()) {
            double r = static_cast<double>(rand()) / static_cast<double>(RAND_MAX);
            size_t pick = 0;
            for (; pick + 1 < multilink_cum_ratios_.size(); ++pick) {
                if (r <= multilink_cum_ratios_[pick]) break;
            }
            target_path = multilink_paths_[pick];
        } else {
            target_path = active_path.load(std::memory_order_relaxed);
        }
    } else {
        target_path = active_path.load(std::memory_order_relaxed);
    }
    if (target_path >= total_link_num) target_path = 0;

    // Record bytes transmitted for path 0
    if (target_path < MAX_PATHS) {
        path_bytes_transmitted[target_path].fetch_add(totalLen, std::memory_order_relaxed);
    }

    LOG_MODULE_INFO(MODULE_SENDER, "[PDCP Sender] Packet seq=" << seq
                    << " assigned to queue=" << static_cast<int>(queue_id)
                    << " via path=" << static_cast<int>(target_path));

    // Send a probe packet on backup link(s) every N packets so CCA on each link
    // gets per-link signal even when scheduler picks single-link dominantly.
    // Without this the unselected link stays "cold" — measured BW=0, sticky-floored
    // to last-known, causing variance/bimodal lock-in. Period configurable via
    // QCON_PROBE_PERIOD env (default 200 = roughly 5 Hz at typical 1Mbps probe load).
    static const int probe_period = []{
        const char* e = std::getenv("QCON_PROBE_PERIOD");
        int p = 0;  // default OFF — probe didn't improve net KPI in 5-trial test
        if (e) { try { p = std::stoi(e); } catch (...) {} }
        return std::max(0, p);  // 0 disables
    }();
    bool should_probe = (probe_period > 0) && (seq % probe_period == 0);

    // Handle packet duplication if enabled
    bool duplication_enabled = duplication_enabled_.load(std::memory_order_relaxed);
    if (duplication_enabled && forwardingCallback) {
        std::vector<uint8_t> paths;
        {
            paths = duplication_paths_;
        }
        
        if (!paths.empty()) {
            LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] Duplicating packet (seq=" << seq << ") to " << paths.size() << " paths");
            
            // Duplicate the packet to all configured paths
            for (uint8_t path : paths) {
                if (path != target_path && path < total_link_num) {
                    // Create a duplicate packet
                    unsigned char* duplicate = new unsigned char[totalLen];
                    std::memcpy(duplicate, packet, totalLen);
                    
                    // Forward on the duplicate path
                    bool success = forwardingCallback(duplicate, totalLen, path);
                    
                    if (success) {
                        // Record bytes transmitted for this path too
                        path_bytes_transmitted[path].fetch_add(totalLen, std::memory_order_relaxed);
                        LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] Duplicated packet sent on path " << static_cast<int>(path));
                    } else {
                        LOG_MODULE_WARN(MODULE_SENDER,"[PDCP Sender] Failed to send duplicate packet on path " << static_cast<int>(path));
                    }
                    
                    // Clean up the duplicate
                    delete[] duplicate;
                }
            }
        }
    } // Send probe dummy packets if duplication is not enabled but probing is needed
    else if (!duplication_enabled && forwardingCallback && should_probe) {
        LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] Sending probe dummy packets for sequence " << seq << " to all paths");
        
        // Send to all available paths except the target one
        for (uint8_t path = 0; path < total_link_num; path++) {
            if (path != target_path) {
                // Create a duplicate packet for probing
                unsigned char* probe_packet = new unsigned char[totalLen];
                std::memcpy(probe_packet, packet, totalLen);
                
                // Forward on this path
                bool success = forwardingCallback(probe_packet, totalLen, path);
                
                if (success) {
                    // Note: We don't increment path_bytes_transmitted for probe packets
                    LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] Probe packet sent on path " << static_cast<int>(path));
                } else {
                    LOG_MODULE_WARN(MODULE_SENDER,"[PDCP Sender] Failed to send probe packet on path " << static_cast<int>(path));
                }
                
                // Clean up
                delete[] probe_packet;
            }
        }
    }

    // Re-inject burst: while remaining > 0, also send a HIGH-priority copy
    // on the alt path. Approximates paper §5.4 packet move; duplication is
    // a setup-specific concession because emulator lacks LCID-aware RLC
    // reorder buffer (true handover would lose packets to seq mismatch).
    uint32_t burst_left = reinject_burst_remaining_.load(std::memory_order_acquire);
    if (burst_left > 0 && forwardingCallback) {
        uint8_t alt = reinject_alt_path_.load(std::memory_order_acquire);
        if (alt < total_link_num && alt != target_path) {
            unsigned char* hi_copy = new unsigned char[totalLen];
            std::memcpy(hi_copy, packet, totalLen);
            // Override pad to highest priority queue (3 = HIGH LCID)
            pdcp_hdr* hi_hdr = reinterpret_cast<pdcp_hdr*>(hi_copy);
            hi_hdr->pad = 3;
            bool ok = forwardingCallback(hi_copy, totalLen, alt);
            if (ok) {
                path_bytes_transmitted[alt].fetch_add(totalLen, std::memory_order_relaxed);
                reinject_burst_remaining_.fetch_sub(1, std::memory_order_release);
            }
            delete[] hi_copy;
        }
    }

    // Use the forwarding callback if available
    if (forwardingCallback) {
        bool success = forwardingCallback(packet, totalLen, target_path);
        if (success) {
            return 0;
        }
    }

    // 8) Return the new total length
    LOG_MODULE_DEBUG(MODULE_SENDER,"PDCP encapsulated packet length: " << totalLen);
    return totalLen;
}

uint64_t PdcpSender::getSequence() const {
    return nextSequence.load(std::memory_order_relaxed);
}

// Implement the new methods in pdcp_sender.cpp
void PdcpSender::setDuplication(bool enabled) {
    duplication_enabled_.store(enabled, std::memory_order_relaxed);
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Packet duplication " << (enabled ? "enabled" : "disabled"));
}

bool PdcpSender::isDuplicationEnabled() const {
    return duplication_enabled_.load(std::memory_order_relaxed);
}

void PdcpSender::setDuplicationPaths(const std::vector<uint8_t>& paths) {
    duplication_paths_ = paths;
    
    // Log the paths
    std::string paths_str;
    for (auto path : duplication_paths_) {
        if (!paths_str.empty()) paths_str += ", ";
        paths_str += std::to_string(static_cast<int>(path));
    }
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Duplication paths set to: [" << paths_str << "]");
}

std::vector<uint8_t> PdcpSender::getDuplicationPaths() const {
    return duplication_paths_;
}

void PdcpSender::ricHelloThread() {
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] RIC hello thread started");
    
    // Counter for periodic messages
    int counter = 0;
    
    while (ric_thread_running_) {
        try {
            // Sleep for 1 second
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            if (!ric_thread_running_) {
                break;
            }
            
            // Send hello world message every 5 seconds
            if (++counter % 5 == 0) {
                // Create a simple JSON message
                struct json_object* hello_obj = json_object_new_object();
                json_object_object_add(hello_obj, "message", json_object_new_string("Hello from PDCP Sender"));
                json_object_object_add(hello_obj, "counter", json_object_new_int(counter));
                json_object_object_add(hello_obj, "sequence", json_object_new_int(nextSequence.load()));
                json_object_object_add(hello_obj, "epoch", json_object_new_int(currentEpoch.load()));
                
                const char* hello_str = json_object_to_json_string(hello_obj);
                
                // Send as a KPM metric
                ric_interface_->sendKpmMetric("hello_world", hello_str);
                
                LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Sent hello world to RIC: " << hello_str);
                
                // Clean up
                json_object_put(hello_obj);
            }
            
            // Send some basic statistics every second
            uint64_t seq = nextSequence.load();
            uint64_t epoch = currentEpoch.load();
            
            std::string stats = std::to_string(seq) + "," + std::to_string(epoch);
            ric_interface_->sendKpmMetric("pdcp_stats", stats);
        }
        catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Exception in RIC hello thread: " << e.what());
            // Continue running after brief pause
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] RIC hello thread stopped");
}

void PdcpSender::kpmMetricsThread() {
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] KPM metrics thread started");
    
    while (kpm_thread_running_) {
        try {
            // Sleep for the configured interval
            std::this_thread::sleep_for(std::chrono::milliseconds(kpm_report_interval_ms.load()));
            
            if (!kpm_thread_running_) {
                break;
            }
            
            // Get current timestamp
            auto now = std::chrono::system_clock::now();
            auto now_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
            
            // Collect path statistics
            struct json_object* kpm_obj = json_object_new_object();
            
            // Add timestamp
            json_object_object_add(kpm_obj, "timestamp", json_object_new_int64(now_ms));
            
            // Add active path
            json_object_object_add(kpm_obj, "active_path", json_object_new_int(active_path.load()));
            json_object_object_add(kpm_obj, "duplication_enabled", json_object_new_boolean(duplication_enabled_.load()));
            
            // Add path byte statistics
            struct json_object* path_stats = json_object_new_object();
            for (int i = 0; i < total_link_num; i++) {
                std::string path_key = "path_" + std::to_string(i);
                json_object_object_add(path_stats, path_key.c_str(), 
                                      json_object_new_int64(path_bytes_transmitted[i].load()));
                //path_bytes_transmitted[i].store(0, std::memory_order_relaxed); // Reset after reporting
            }
            json_object_object_add(kpm_obj, "path_bytes", path_stats);
            
            // Add basic sequence info
            json_object_object_add(kpm_obj, "sequence", json_object_new_uint64(nextSequence.load()));
            json_object_object_add(kpm_obj, "epoch", json_object_new_uint64(currentEpoch.load()));
            json_object_object_add(kpm_obj, "message", json_object_new_string(("CU")));
            
            // Convert to JSON string
            const char* kpm_str = json_object_to_json_string(kpm_obj);
            
            // Send KPM metric
            if (ric_interface_) {
                ric_interface_->sendKpmMetric("pdcp_path_stats", kpm_str);
                LOG_MODULE_DEBUG(MODULE_SENDER,"[PDCP Sender] Sent path statistics to RIC: " << kpm_str);
            }
            
            // Clean up
            json_object_put(kpm_obj);
            
            // Update last report time
            last_kpm_report_time_ = std::chrono::steady_clock::now();
        }
        catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Exception in KPM metrics thread: " << e.what());
            // Continue running after brief pause
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] KPM metrics thread stopped");
}

void PdcpSender::registerRicCommandHandlers() {
    if (!ric_interface_) {
        LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Cannot register command handlers - RIC interface not initialized");
        return;
    }
    
    // Register handler for setting active path
    ric_interface_->registerCommandHandler("set_path", [this](const std::string& params) -> std::string {
        return this->handleSetPath(params);
    });

    // Register handler for enabling/disabling packet duplication
    ric_interface_->registerCommandHandler("set_duplication", [this](const std::string& params) -> std::string {
        return this->handleSetDuplication(params);
    });

    // Paper §5.4 priority-based re-inject: duplicate next N packets onto alt
    // path with HIGH priority queue. Triggered by RIC when current link is at
    // risk of missing the deadline AND alt link could absorb the work.
    ric_interface_->registerCommandHandler("reinject_burst", [this](const std::string& params) -> std::string {
        return this->handleReinjectBurst(params);
    });

    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Registered RC command handlers");
}

std::string PdcpSender::handleReinjectBurst(const std::string& params) {
    // Params expected: { "alt_path": <int>, "burst_packets": <int> }
    struct json_object* response = json_object_new_object();
    int alt = 0, burst = 30;
    struct json_object* params_obj = json_tokener_parse(params.c_str());
    if (params_obj) {
        struct json_object* obj = nullptr;
        if (json_object_object_get_ex(params_obj, "alt_path", &obj))
            alt = json_object_get_int(obj);
        if (json_object_object_get_ex(params_obj, "burst_packets", &obj))
            burst = json_object_get_int(obj);
        json_object_put(params_obj);
    }
    if (alt < 0 || alt >= (int)total_link_num) {
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message",
                               json_object_new_string("alt_path out of range"));
    } else {
        reinject_alt_path_.store(static_cast<uint8_t>(alt), std::memory_order_release);
        reinject_burst_remaining_.store(static_cast<uint32_t>(burst), std::memory_order_release);
        // ERROR-level so it shows up regardless of log level config — burst is a
        // visible data-plane action and we want it auditable in every run.
        LOG_MODULE_ERROR(MODULE_SENDER, "[PDCP Sender] Re-inject burst: alt_path="
                        << alt << " burst_packets=" << burst);
        json_object_object_add(response, "status", json_object_new_string("ok"));
    }
    std::string out = json_object_to_json_string(response);
    json_object_put(response);
    return out;
}

std::string PdcpSender::handleSetPath(const std::string& params) {
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Processing set_path command with params: " << params);

    struct json_object* response = json_object_new_object();

    try {
        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        // Add timestamps to response
        json_object_object_add(response, "timestamp", json_object_new_int64(now_ms));

        // Parse parameters
        struct json_object* params_obj = json_tokener_parse(params.c_str());
        if (!params_obj) {
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string("Invalid JSON parameters"));
        } else {
            // Multilink branch (sent by CuSchedulerController::sendMultilinkCommandImmediate):
            // { "is_multilink": true, "link_ids": [0-indexed], "split_ratios": [...] }
            struct json_object* ml_obj = nullptr;
            bool is_ml = false;
            if (json_object_object_get_ex(params_obj, "is_multilink", &ml_obj)) {
                is_ml = json_object_get_boolean(ml_obj);
            }
            struct json_object* lid_arr = nullptr;
            struct json_object* rat_arr = nullptr;
            if (is_ml &&
                json_object_object_get_ex(params_obj, "link_ids", &lid_arr) &&
                json_object_object_get_ex(params_obj, "split_ratios", &rat_arr) &&
                json_object_is_type(lid_arr, json_type_array) &&
                json_object_is_type(rat_arr, json_type_array)) {

                int n = std::min(json_object_array_length(lid_arr),
                                 json_object_array_length(rat_arr));
                std::vector<uint8_t> ml_paths;
                std::vector<double>  ml_rats;
                for (int i = 0; i < n; ++i) {
                    int lid = json_object_get_int(json_object_array_get_idx(lid_arr, i));
                    double r = json_object_get_double(json_object_array_get_idx(rat_arr, i));
                    if (lid >= 0 && lid < (int)total_link_num && r >= 0.0) {
                        ml_paths.push_back(static_cast<uint8_t>(lid));
                        ml_rats.push_back(r);
                    }
                }
                double sum = 0.0;
                for (double r : ml_rats) sum += r;
                if (!ml_paths.empty() && sum > 0.0) {
                    std::vector<double> cdf;
                    double acc = 0.0;
                    for (double r : ml_rats) { acc += r / sum; cdf.push_back(acc); }
                    {
                        std::lock_guard<std::mutex> g(multilink_state_mutex_);
                        multilink_paths_      = std::move(ml_paths);
                        multilink_cum_ratios_ = std::move(cdf);
                    }
                    multilink_active_.store(true, std::memory_order_release);
                    LOG_MODULE_INFO(MODULE_SENDER, "[PDCP Sender] Multilink ON ("
                        << multilink_paths_.size() << " paths)");

                    json_object_object_add(response, "status", json_object_new_string("success"));
                    json_object_object_add(response, "message",
                        json_object_new_string("multilink path applied"));
                    json_object_put(params_obj);
                    const char* resp = json_object_to_json_string(response);
                    std::string result(resp);
                    json_object_put(response);
                    return result;
                }
            }
            // Falling through to single-link path: disable multilink mode.
            multilink_active_.store(false, std::memory_order_release);

            // Extract path parameter
            struct json_object* path_obj = nullptr;
            if (json_object_object_get_ex(params_obj, "path", &path_obj)) {
                int path = json_object_get_int(path_obj);
                
                // Validate path value
                if (path >= 0 && path < total_link_num) {
                    // Update the active path
                    uint8_t old_path = active_path.load(std::memory_order_relaxed);
                    active_path.store(static_cast<uint8_t>(path), std::memory_order_relaxed);
                    
                    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Changed active path from " << (int)old_path << " to " << path);
                    
                    // Add success information to response
                    json_object_object_add(response, "status", json_object_new_string("success"));
                    json_object_object_add(response, "message", 
                                         json_object_new_string(("Active path set to " + std::to_string(path)).c_str()));
                    json_object_object_add(response, "previous_path", json_object_new_int(old_path));
                    json_object_object_add(response, "new_path", json_object_new_int(path));
                } else {
                    LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Invalid path value: " << path << 
                             ". Must be between 0 and " << (total_link_num-1));
                    
                    json_object_object_add(response, "status", json_object_new_string("error"));
                    json_object_object_add(response, "message", 
                                         json_object_new_string(
                                            ("Invalid path value. Must be between 0 and " + 
                                            std::to_string(total_link_num-1)).c_str()));
                }
            } else {
                LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Missing 'path' parameter in command");
                
                json_object_object_add(response, "status", json_object_new_string("error"));
                json_object_object_add(response, "message", json_object_new_string("Missing 'path' parameter"));
            }


            struct json_object* timestamp_obj = nullptr;
            if (json_object_object_get_ex(params_obj, "timestamp", &timestamp_obj)) {
                int64_t sender_timestamp = json_object_get_int64(timestamp_obj);
                json_object_object_add(response, "sender_timestamp", json_object_new_int64(sender_timestamp));
            }
            
            json_object_put(params_obj);
        }
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Exception in handleSetPath: " << e.what());
        
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", 
                             json_object_new_string(("Exception: " + std::string(e.what())).c_str()));
    }
    
    // Convert to string
    const char* response_str = json_object_to_json_string(response);
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Response: " << response_str << " for params: " << params);
    std::string result(response_str);
    
    // Clean up
    json_object_put(response);
    
    return result;
}

// Implement the new command handler
std::string PdcpSender::handleSetDuplication(const std::string& params) {
    LOG_MODULE_INFO(MODULE_SENDER,"[PDCP Sender] Processing set_duplication command with params: " << params);
    
    struct json_object* response = json_object_new_object();
    
    try {
        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        // Add timestamps to response
        json_object_object_add(response, "timestamp", json_object_new_int64(now_ms));
        
        // Parse parameters
        struct json_object* params_obj = json_tokener_parse(params.c_str());
        if (!params_obj) {
            json_object_object_add(response, "status", json_object_new_string("error"));
            json_object_object_add(response, "message", json_object_new_string("Invalid JSON parameters"));
        } else {
            // Extract enabled parameter
            struct json_object* enabled_obj = nullptr;
            bool enabled = false;
            bool has_enabled = false;
            
            if (json_object_object_get_ex(params_obj, "enabled", &enabled_obj)) {
                enabled = json_object_get_boolean(enabled_obj);
                has_enabled = true;
            }
            
            // Extract paths parameter
            struct json_object* paths_obj = nullptr;
            std::vector<uint8_t> paths;
            bool has_paths = false;
            
            if (json_object_object_get_ex(params_obj, "paths", &paths_obj) && 
                json_object_is_type(paths_obj, json_type_array)) {
                
                int path_count = json_object_array_length(paths_obj);
                for (int i = 0; i < path_count; i++) {
                    struct json_object* path_item = json_object_array_get_idx(paths_obj, i);
                    if (json_object_is_type(path_item, json_type_int)) {
                        int path = json_object_get_int(path_item);
                        if (path >= 0 && path < total_link_num) {
                            paths.push_back(static_cast<uint8_t>(path));
                        } else {
                            LOG_MODULE_WARN(MODULE_SENDER,"[PDCP Sender] Invalid path value in array: " << path);
                        }
                    }
                }
                has_paths = true;
            }
            
            // Apply the changes
            if (has_enabled) {
                setDuplication(enabled);
            }
            
            if (has_paths) {
                setDuplicationPaths(paths);
            }
            
            // Build response
            if (has_enabled || has_paths) {
                json_object_object_add(response, "status", json_object_new_string("success"));
                
                std::string message = "Duplication ";
                if (has_enabled) {
                    message += std::string(enabled ? "enabled" : "disabled");
                }
                
                if (has_paths) {
                    if (has_enabled) {
                        message += " and ";
                    }
                    
                    message += "paths updated";
                }
                
                json_object_object_add(response, "message", json_object_new_string(message.c_str()));
                
                // Add current settings to response
                json_object_object_add(response, "duplication_enabled", 
                                     json_object_new_boolean(isDuplicationEnabled()));
                
                // Add paths to response
                std::vector<uint8_t> current_paths = getDuplicationPaths();
                struct json_object* current_paths_obj = json_object_new_array();
                
                for (uint8_t path : current_paths) {
                    json_object_array_add(current_paths_obj, json_object_new_int(path));
                }
                
                json_object_object_add(response, "duplication_paths", current_paths_obj);
            } else {
                json_object_object_add(response, "status", json_object_new_string("error"));
                json_object_object_add(response, "message", 
                                     json_object_new_string("No valid parameters provided"));
            }

            // Add sender timestamp if provided
            struct json_object* timestamp_obj = nullptr;
            if (json_object_object_get_ex(params_obj, "timestamp", &timestamp_obj)) {
                int64_t sender_timestamp = json_object_get_int64(timestamp_obj);
                json_object_object_add(response, "sender_timestamp", json_object_new_int64(sender_timestamp));
            }
            
            json_object_put(params_obj);
        }
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE_SENDER,"[PDCP Sender] Exception in handleSetDuplication: " << e.what());
        
        json_object_object_add(response, "status", json_object_new_string("error"));
        json_object_object_add(response, "message", 
                             json_object_new_string(("Exception: " + std::string(e.what())).c_str()));
    }
    
    // Convert to string
    const char* response_str = json_object_to_json_string(response);
    std::string result(response_str);

    // Clean up
    json_object_put(response);

    return result;
}

// QoS Profiler methods
void PdcpSender::setQosMode(QosProfiler::ClassificationMode mode) {
    if (qos_profiler_) {
        qos_profiler_->setMode(mode);
        LOG_MODULE_INFO(MODULE_SENDER, "[PDCP Sender] QoS classification mode set to "
                        << static_cast<int>(mode));
    }
}

QosProfiler::ClassificationMode PdcpSender::getQosMode() const {
    if (qos_profiler_) {
        return qos_profiler_->getMode();
    }
    return QosProfiler::ClassificationMode::SEQUENCE_MOD;
}

uint64_t PdcpSender::getQueueStats(uint8_t queueId) const {
    if (qos_profiler_) {
        return qos_profiler_->getQueueStats(queueId);
    }
    return 0;
}

void PdcpSender::resetQosStats() {
    if (qos_profiler_) {
        qos_profiler_->resetStats();
        LOG_MODULE_INFO(MODULE_SENDER, "[PDCP Sender] QoS statistics reset");
    }
}