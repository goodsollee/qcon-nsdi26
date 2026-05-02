#ifndef QOE_PROCESSOR_HPP
#define QOE_PROCESSOR_HPP

#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <mutex>
#include <memory>
#include <jsoncpp/json/json.h>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <queue>
#include <functional>
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <fstream> 
#include <set>
#include "async_logger.hpp"
#include "RIC/zmq_interface.hpp"

#define VIDEO_PAYLOAD_TYPE 112 // VP8/VP9: 106, H264: 127

namespace ric {

// RTP header structure
struct RtpHeader {
    uint8_t  first_byte;       // version/padding/extension/cc
    uint8_t  second_byte;      // marker (bit 7) + payload type (bits 0..6)
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
};

// RTP packet metadata
struct PacketInfo {
    uint32_t timestamp;
    uint16_t sequence_number;
    uint8_t  marker;
    uint8_t  payload_type;
    uint8_t  is_rtp;
    uint8_t  _pad;          // keep layout identical
    uint32_t packet_size;
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t _pad16;
    uint32_t arrival_time_ms;
    uint32_t total_len;
    uint32_t dst_ip;       // Destination IP address
    uint16_t dst_port;     // Destination port
    uint16_t _pad16_2;     // padding for alignment
};

// Frame delivery status
enum class FrameDeliveryStatus {
    IN_PROGRESS,
    COMPLETE,
    TIMEOUT
};


// Frame RLC progress tracking
struct FrameRlcInfo {
    std::unordered_map<int, uint64_t> rlc_bytes_to_be_arrived;
    std::unordered_map<int, uint64_t> rlc_bytes_acked;
};

// Frame information
struct FrameInfo {
    uint32_t timestamp;
    std::chrono::steady_clock::time_point start_time;           // First packet arrival time
    std::chrono::steady_clock::time_point last_packet_time;     // Last packet arrival time
    std::chrono::steady_clock::time_point marker_arrival_time;  // When the marker packet arrived
    std::vector<uint16_t> sequence_numbers;
    std::vector<bool> packets_received;
    uint32_t total_size;
    uint32_t received_size;
    uint32_t required_size;
    FrameDeliveryStatus status;
    double frame_delivery_progress;
    bool has_marker;
    FrameRlcInfo frame_rlc_info_;
    std::chrono::milliseconds delivery_time;                    // Overall delivery time
    
    // RLC ACK tracking for UE frame construction time
    std::chrono::steady_clock::time_point first_rlc_ack_time;   // First RLC ACK for this frame
    std::chrono::steady_clock::time_point last_rlc_ack_time;    // Last RLC ACK for this frame
    bool has_rlc_ack_start;                                     // Whether first RLC ACK was recorded
    bool has_rlc_ack_end;                                       // Whether last RLC ACK was recorded

    bool all_txed;

    bool fully_acked = false;   // 모든 링크 ACK 완료 여부
    bool reinjected_to_alt = false;  // sweep dedup: true once unacked sent to alt
    std::chrono::steady_clock::time_point last_activity_time;
    
    // User identification - based on packet destination
    uint32_t dst_ip;                                           // Destination IP address
    std::string user_id;                                       // User ID (derived from dst_ip)
};

/**
 * QoEProcessor class
 * This class processes RTP packets to extract QoE metrics for real-time streaming
 */
// Forward declaration
class ZmqInterface;

class QoEProcessor {
public:
    QoEProcessor();
    ~QoEProcessor();

    /**
     * Initialize the QoE processor
     * @param interface Name of the network interface to monitor
     * @param test_mode If true, run in test mode without actually loading BPF
     * @return true if initialization was successful, false otherwise
     */
    bool initialize(const std::string& interface, bool test_mode = false);

    /**
     * Start processing RTP packets
     * @return true if started successfully, false otherwise
     */
    bool start();

    /**
     * Stop processing RTP packets
     */
    void stop();

    /**
     * Get the current QoE metrics
     * @return Json::Value with QoE metrics
     */
    Json::Value getQoEMetrics() const;

    /**
     * Get frame delivery progress for a specific timestamp
     * @param timestamp RTP timestamp of the frame
     * @return Frame delivery progress (0.0 to 1.0)
     */
    double getFrameDeliveryProgress(uint32_t timestamp) const;

    /**
     * Get the estimated deadline for a frame
     * @param timestamp RTP timestamp of the frame
     * @return Estimated deadline in milliseconds
     */
    int64_t getFrameDeadline(uint32_t timestamp) const;

    /**
     * Get frame jitter statistics
     * @return Json::Value with jitter statistics
     */
    Json::Value getJitterStats() const;

    /**
     * Set a callback for frame completion events
     * @param callback Function to call when a frame is complete
     */
    void setFrameCompletionCallback(std::function<void(uint32_t, std::chrono::milliseconds)> callback);
    
    /**
     * Set a callback for new frame arrival events
     * @param callback Function to call when a new frame arrives
     */
    void setNewFrameCallback(std::function<void()> callback);
    
    /**
     * Update frame progress based on RLC metrics
     * @param rlc_acked_bytes Total RLC acknowledged bytes
     * @param link_id Identifier for the link that provided this update
     * @return True if update was successful
     */
    bool updateTxMetrics(uint64_t pdcp_transmitted_bytes, uint64_t rlc_acked_bytes, int link_id);
    
    /**
     * Get a list of active frame timestamps in arrival order
     * @return Vector of frame timestamps in arrival order
     */
    std::unordered_map<uint32_t, FrameInfo> getActiveFrames() const;
    
    /**
     * Get active frames for a specific user
     * @param user_id User identifier to get frames for
     * @return Map of frame timestamps to frame info for the specified user
     */
    std::unordered_map<uint32_t, FrameInfo> getActiveFramesForUser(const std::string& user_id) const;
    
    /**
     * Get a list of all active users
     * @return Vector of active user IDs
     */
    std::vector<std::string> getActiveUsers() const;

    void saveSchedulingInfo (std::map<int, double> split_ratios);

    /**
     * Recent-window frame statistics — paper §5.3 specifies QCON Algorithm 1
     * inputs are derived from the average recent frame size and the observed
     * frame rate over a 100 ms window. These methods take a snapshot of frames_
     * filtered by arrival time and compute size/rate without depending on the
     * RLC-ack frame completion pipeline (which is brittle in the emulator).
     *
     * @param window_ms sliding window size in milliseconds (100 = paper)
     * @return average bytes per frame across frames whose start_time ≥ now - window
     */
    double getRecentFrameSizeBytes(uint32_t window_ms = 100) const;
    double getRecentFrameRateFps(uint32_t window_ms = 100) const;

    // Real-time inflight-frame snapshot for paper §5.4 priority re-injection.
    // Each entry: rtp timestamp, frame.start_time (ms since epoch), total bytes
    // of the frame, bytes already RLC-acked across all links, and the link
    // currently carrying this frame's bulk (largest target_bytes link).
    // Scheduler iterates this list per tick to detect when a frame is at risk
    // of missing its deadline and trigger reinject_burst to the alt link.
    struct InflightFrameSnapshot {
        uint32_t  rtp_ts;
        int64_t   start_time_ms;     // monotonic ms since epoch
        uint64_t  total_bytes;
        uint64_t  acked_bytes;       // sum across all links
        int       primary_link_id;   // 0-indexed link with most bytes assigned
    };
    std::vector<InflightFrameSnapshot> getInflightFrameSnapshots() const;

    // Paper §5.3 inputs ── single most-recent completed frame's delivery time
    // (T_{m-1}) and measured per-frame jitter (J_on).
    //
    // getLastCompletedFrameDeliveryMs(): returns the delivery_time (ms) of the
    // last frame that reached COMPLETE status. 0 if no frame has completed.
    // This is the actual T_{m-1} for eq3, NOT a sliding-window average.
    //
    // getRecentMaxFrameJitterMs(): returns max(|inter_completion_diff -
    // inter_arrival_diff|) over the last `window_n` completed frames. This is
    // J_on per paper §5.3 — observed maximum jitter, lets J_th=max(J_on,J_off)
    // grow when network is jittery, stay at J_off when smooth.
    double getLastCompletedFrameDeliveryMs() const;
    double getRecentMaxFrameJitterMs(uint32_t window_n = 30) const;

    // Zombie-frame sweep (paper §5.4 extension). When a NEW frame arrives, every
    // OTHER frame still in IN_PROGRESS status is, by definition, late: the
    // upper layer has moved on. We classify each:
    //   - acked == total → ACK pipeline completed but marker stuck. Promote to
    //     COMPLETE so frame_delays / T_{m-1} reflect reality.
    //   - acked  < total → un-ACKed bytes on a (likely dead) link. Return to
    //     caller so it can reinject the un-ACKed portion to alt link, then
    //     mark TIMEOUT to cycle the frame out of IN_PROGRESS.
    // Either way the frame is removed from the inflight set so the scheduler
    // never sees it as a zombie next tick.
    struct AtRiskFrame {
        uint32_t rtp_ts;
        uint64_t unacked_bytes;
        int      primary_link_id;        // 0-indexed link that was carrying this
        int64_t  elapsed_ms;             // wall time since first packet
        bool     already_reinjected;     // dedup across calls
    };
    // Returns every IN_PROGRESS frame (except `newest_ts`) with its current
    // un-ACKed-byte count. Side effects:
    //   - frames with unacked == 0 are promoted to COMPLETE (frame_delays gets
    //     a real number, T_{m-1} reflects truth).
    //   - frames with elapsed > deadline_ms are promoted to TIMEOUT (cleanup;
    //     they no longer linger as zombies next sweep).
    // The returned frames cover BOTH preemptive cases (still inside deadline
    // but un-ACKed) AND past-deadline cases — caller picks alt link, decides
    // who to reinject (using cur_bw vs alt_bw vs slack), and calls
    // markFrameReinjected so the next sweep doesn't fire the same burst again.
    std::vector<AtRiskFrame> sweepInProgressFrames(uint32_t newest_ts,
                                                    int64_t  deadline_ms);
    void markFrameReinjected(uint32_t rtp_ts);
    uint32_t getNewestFrameTs() const;

    uint32_t recent_timestamp_;
    std::map<int,double> recent_split_ratios_;
    
    // Set ZMQ interface for communication with CU
    void setZmqInterface(std::shared_ptr<ZmqInterface> zmq_interface) {
        zmq_interface_ = zmq_interface;
    }

private:
    std::unique_ptr<AsyncLogger> csvLogger_;
    std::shared_ptr<ZmqInterface> zmq_interface_;  

    // User tracking
    mutable std::mutex users_mutex_;
    std::unordered_map<uint32_t, std::string> ip_to_user_id_; // Map IP to user ID
    std::unordered_map<std::string, std::unordered_map<uint32_t, FrameInfo>> user_frames_; // Frames per user

    // BPF program and maps
    struct bpf_object *obj;
    struct bpf_program *prog;
    struct bpf_link *link;
    int prog_fd;
    int ringbuf_fd;
    
    // Processing thread
    std::thread processing_thread;
    std::atomic<bool> running;
    
    // Frame tracking
    mutable std::mutex frames_mutex;
    std::unordered_map<uint32_t, FrameInfo> frames;
    // Vector to maintain frame arrival order
    std::vector<uint32_t> frame_arrival_order;    
    
    // Frame statistics logging
    std::string log_dir_;
    std::ofstream frame_stats_file_;
    std::mutex frame_stats_mutex_;
    
    // For tracking frame-level jitter
    uint32_t last_frame_timestamp_;
    std::chrono::steady_clock::time_point last_frame_arrival_time_;
    std::chrono::steady_clock::time_point last_frame_completion_time_;
    
    // Network interface
    std::string interface_name;
    
    // QoE metrics
    struct QoEMetrics {
        std::chrono::steady_clock::time_point last_update;
        double avg_frame_rate;
        double avg_bitrate;
        double avg_frame_delay;
        double avg_jitter;
        double packet_loss_rate;
        
        // RLC metrics
        uint64_t rlc_acked_bytes;
        double rlc_throughput_mbps;
    };
    
    mutable std::mutex metrics_mutex;
    QoEMetrics current_metrics;
    
    // Callbacks
    std::function<void(uint32_t, std::chrono::milliseconds)> frame_completion_callback;
    std::function<void()> new_frame_callback;
    
    // Statistics variables
    std::chrono::steady_clock::time_point stats_start_time;
    std::chrono::steady_clock::time_point stats_last_update_time;
    uint64_t total_frames_completed;
    uint64_t total_frames_timeout;
    uint64_t total_packets_received;
    uint64_t total_packets_expected;
    uint64_t elapsed_frames_completed;
    uint64_t elapsed_bytes_received;
    
    // RLC metrics per link
    std::chrono::steady_clock::time_point last_rlc_update_time_;
    uint64_t last_reported_rlc_bytes_;
    std::unordered_map<int, uint64_t> link_rlc_acked_bytes_; // Track acked bytes per link
    
    // Deadline estimation
    const std::chrono::milliseconds DEFAULT_FRAME_DEADLINE{33}; // ~30fps
    std::chrono::milliseconds estimated_frame_interval;
    
    // Jitter calculation
    std::deque<std::chrono::milliseconds> frame_delays;
    const size_t MAX_FRAME_DELAY_HISTORY = 30;
    
    // Statistics update interval
    const std::chrono::milliseconds STATS_UPDATE_INTERVAL{100};
    std::chrono::steady_clock::time_point last_stats_update;
    
    // Frame update notification thread
    std::thread frame_update_thread_;
    std::atomic<bool> frame_update_thread_running_{false};
    std::mutex frame_update_queue_mutex_;
    std::condition_variable frame_update_cv_;
    struct FrameUpdateInfo {
        uint32_t timestamp;
        std::string event_type;
        std::chrono::steady_clock::time_point scheduled_time;
    };
    std::queue<FrameUpdateInfo> frame_update_queue_;
    void frameUpdateThread();
    void scheduleFrameUpdate(uint32_t timestamp, bool is_marker, int delay_ms = 0);
    
    // Internal methods
    void processingThread();
    void processRtpPacket(const PacketInfo& info);
    void updateFrameStatus(uint32_t timestamp);
    void updateQoEMetrics();
    void cleanupOldFrames();
    void updateFrameTxed(int link_id, uint64_t txed_bytes);
    void updateFrameProgress(int link_id, uint64_t acked_bytes);
    
    // Frame management
    void addFramePacket(uint32_t timestamp, uint16_t seq_num, uint32_t size, bool is_marker, 
                       uint32_t dst_ip = 0, const std::string& user_id = "");
    void scheduleFrameCallback(uint32_t timestamp, int delay_ms);
    void completeFrame(uint32_t timestamp);
    void timeoutFrame(uint32_t timestamp);
    double calculateFrameProgress(const FrameInfo& frame) const;
    std::chrono::milliseconds calculateJitter() const;

    std::atomic<uint32_t> oldest_inflight_{0};   // the smallest ts in inflight
    std::set<uint32_t>    ready_frames_;         // ready frames
    mutable std::mutex    ready_mtx_;            // mutex for ready_frames_
    void advanceOldestInflight();                // get the oldest inflight frame
    void drainReadyFrames(std::vector<uint32_t>& out); // drain ready frames

    uint64_t total_rlc_bytes_received = 0; // Total bytes received
    uint64_t total_app_bytes_received = 0;
    uint64_t total_pdcp_bytes_received = 0;
    
    uint64_t pdcp_diff_sum = 0;
    uint64_t rlc_diff_sum = 0;

    uint64_t total_bytes_sniffed_     = 0;   // every packet
    uint64_t video_bytes_sniffed_     = 0;   // RTP with payload_type == VIDEO_PAYLOAD_TYPE
    uint64_t other_bytes_sniffed_     = 0;   // anything else
    
    std::unordered_map<int, uint64_t> last_pdcp_txed_total_;
    std::unordered_map<int, uint64_t> last_rlc_acked_total_;

    bool isFullyAcked(const FrameRlcInfo& rlc) const;

    std::unordered_map<int, uint64_t> pending_ack_credit_;



};

} // namespace ric

#endif // QOE_PROCESSOR_HPP