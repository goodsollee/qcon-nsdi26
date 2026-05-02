#ifndef QCON_SCHEDULER_HPP
#define QCON_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include "RIC/qoe_processor.hpp"
#include "scheduler/QCON/deadline_setting.hpp"
#include <memory>
#include <map>
#include <queue>
#include <chrono>

namespace ric {


// Frame tracking
struct FrameStatus {
    uint32_t timestamp;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point first_arrival_time; // First packet arrival at RIC
    std::chrono::steady_clock::time_point marker_received_time; // When marker packet was received
    std::chrono::steady_clock::time_point completion_time; // When frame was fully transmitted
    std::chrono::milliseconds deadline;
    double progress;
    int assigned_link;
    
    // Metrics tracking
    double estimated_size_kb;       // Size used in scheduling decision
    double actual_size_kb;          // Actual size after marker is received
    double expected_completion_time_ms; // Predicted completion time
    double actual_completion_time_ms;   // Actual completion time
    bool marker_received;           // Whether marker has been received
    bool completed;                 // Whether frame is completed
    
    // Multilink information
    bool is_multilink;
    std::vector<int> link_ids;      // IDs of links used for this frame
    std::vector<double> chunk_sizes_kb; // Chunk sizes for each link
};

/**
 * QconSchedulerConfig - Configuration for the QCON scheduler
 */
struct QconSchedulerConfig {
    // Deadline adjustment parameters
    double jitter_threshold = 10.0;        // Jitter threshold in ms to trigger deadline adjustment
    double deadline_adjustment_factor = 0.8; // How much to reduce deadline when jitter exceeds threshold
    
    // Link selection parameters
    double primary_link_preference = 0.2;  // How much to prefer the primary link when both are suitable
    double backup_link_threshold = 0.7;    // Only use backup link when primary link utilization > threshold
    
    // Packet forwarding parameters
    bool enable_packet_forwarding = true;  // Whether to enable packet forwarding
    double delay_threshold_for_forwarding = 0.6; // Threshold of delay ratio to trigger forwarding
    
    // Multilink scheduling parameters
    bool enable_multilink_scheduling = true; // Whether to enable multilink scheduling
    double avg_frame_size_kb = 50.0;      // Average frame size in KB (for scheduling)
    double min_chunk_size_kb = 1.0;       // Minimum chunk size to assign to a link (in KB)
    double deadline_threshold = 0.9;      // Threshold ratio (legacy, paper Algorithm 1 fires multilink only when single FAILS deadline)
    bool use_cost_based_selection = true; // Whether to consider link_id as cost for link selection
};

struct QconLinkInfo {
    double latest_rtt_ms;       // e.g. from getAverageDelayMs() or getLatestDelayMs()
    double throughput_mbps;  // from getThroughputMbps()
    double buffer_bytes;     // from getBufferOccupancyBytes()
};


/**
 * QconScheduler - QoE-driven multi-connectivity scheduler
 * 
 * This scheduler uses RTP header inspection to extract QoE signals
 * for real-time streaming applications and makes scheduling decisions
 * to enhance application performance.
 */
class QconScheduler : public BaseScheduler {
public:
    QconScheduler();
    virtual ~QconScheduler();
    
    /**
     * Initialize the scheduler
     * 
     * @param state_manager Pointer to the state manager
     * @param config Configuration for the scheduler
     * @return true if initialization was successful, false otherwise
     */
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                  const SchedulerConfig& config) override;
    
    /**
     * Set the QoE processor
     * 
     * @param qoe_processor Pointer to the QoE processor
     */
    void setQoEProcessor(std::shared_ptr<QoEProcessor> qoe_processor);
    
    /**
     * Enable or disable packet forwarding
     * 
     * @param enable Whether to enable packet forwarding
     */
    void enablePacketForwarding(bool enable);
    
    /**
     * Update the QCON scheduler configuration
     * 
     * @param config New configuration
     */
    void updateQconConfig(const QconSchedulerConfig& config);
    
    /**
     * Get the QCON scheduler configuration
     * 
     * @return Current QCON configuration
     */
    QconSchedulerConfig getQconConfig() const;
    
    /**
     * Load configuration from a CSV file
     * 
     * @param csv_path Path to the CSV configuration file
     * @return true if configuration was loaded successfully, false otherwise
     */
    bool loadConfigFromCsv(const std::string& csv_path);
    
    /**
     * Get extended statistics for the QCON scheduler
     * 
     * @return JSON with QCON-specific statistics
     */
    Json::Value getExtendedStatistics() const;

protected:
    /**
     * Make a scheduling decision based on frame delivery progress and link states
     * 
     * @return The scheduling decision
     */
    SchedulingDecision makeDecisionInternal() override;
    
    /**
     * Set up CSV log files for scheduler metrics
     */
    void setupLogFiles();
    
    /**
     * Handle new frame notification from QoE processor
     */
    void handleNewFrame();
    
    /**
     * Synchronize active_frames_ with frames tracked by QoE processor
     */
    void syncFramesWithQoEProcessor();
    
    /**
     * Log frame lifecycle events to CSV
     * 
     * @param frame The frame being logged
     * @param event Event type (new_frame, scheduled, marker_received, completed)
     */
    void logFrameLifecycleEvent(const FrameStatus& frame, const std::string& event);
    
    /**
     * Handle link state updates from the state manager
     * 
     * @param link_id ID of the updated link
     * @param link_state New link state
     */
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;
    
    // Called by the framework (BaseScheduler) to push link metrics. We'll store them.
    void updateLinkMetrics(const std::map<int, LinkMetrics>& metrics) override;

    bool findMinCostMultilinkSolution(
        double frame_size_kb,
        double deadline_ms,
        std::vector<int>& selected_link_ids,
        std::vector<double>& selected_chunk_sizes,
        double& best_completion_time,
        double& total_cost);
        
    std::vector<double> computeOptimalChunkSizes(
        double total_size_kb, 
        const std::vector<double>& bandwidths, 
        const std::vector<double>& rtts);

private:
    // QoE processor for RTP header analysis
    std::shared_ptr<QoEProcessor> qoe_processor_;
    
    // Deadline settings calculator
    DeadlineSettings deadline_settings_;
    
    // QCON-specific configuration
    QconSchedulerConfig qcon_config_;
    

    std::unordered_map<uint32_t, FrameStatus> active_frames_;
    
    // Track frame lifecycle data for more detailed analysis
    std::unique_ptr<AsyncLogger> frame_lifecycle_logger_;
    
    // Store forwarding status
    mutable std::map<int, bool> forwarding_enabled_;
    
    // Statistics
    struct QconStats {
        uint64_t primary_link_selections;
        uint64_t backup_link_selections;
        uint64_t forwarding_events;
        uint64_t deadline_adjustments;
        double avg_frame_completion_time;
        double avg_frame_deadline;
        double avg_jitter;
    };
    
    QconStats stats_;
    
    // CSV logging
    std::string log_dir_;
    std::unique_ptr<AsyncLogger> qoe_logger_;
    std::unique_ptr<AsyncLogger> scheduling_logger_;
    std::unique_ptr<AsyncLogger> deadline_logger_;
    std::mutex log_mutex_;
    
    // Flag to track if frame arrival order is initialized
    bool frame_arrival_order_initialized_;
    
    // Frame history for deadline estimation
    std::queue<std::chrono::milliseconds> frame_completion_times_;
    const size_t MAX_FRAME_HISTORY = 30;
    
    // Link states
    struct LinkMetrics {
        double throughput_mbps;
        double delay_ms;
        int64_t queue_size_bytes;
        double jitter_ms;
        double packet_loss;
        double buffer_occupancy_ratio;
        bool is_congested;
        uint64_t acked_bytes;
    };
    
    std::map<int, LinkMetrics> link_metrics_;
    
    // Multilink scheduling state
    struct LinkAssignment {
        int link_id;
        double chunk_size_kb;
        double expected_completion_time_ms;
    };
    
    struct FrameSchedulingInfo {
        uint32_t timestamp;
        double total_size_kb;
        std::vector<LinkAssignment> link_assignments;
        std::chrono::steady_clock::time_point schedule_time;
    };
    
    std::map<uint32_t, FrameSchedulingInfo> frame_scheduling_;
    
    // Forwarding decisions
    //std::map<int, bool> forwarding_enabled_;
    
    // Helper methods
    void updateDeadlines();
    void updateFrameProgress();
    std::chrono::milliseconds estimateFrameDeadline();
    int selectBestLink(uint32_t frame_timestamp, double frame_progress);
    bool shouldForwardPackets(int link_id);
    void handleFrameCompletion(uint32_t timestamp, std::chrono::milliseconds completion_time);
    
    /**
     * Calculate and update deadline for a frame
     * 
     * @param frame_timestamp The timestamp of the frame
     * @param remaining_bytes_kb Remaining bytes to be transmitted in KB
     * @param last_frame_completion_time_ms Time taken to complete the last frame in ms
     * @return Expected delivery time in milliseconds
     */
    double calculateFrameDeadline(uint32_t frame_timestamp, double remaining_bytes_kb, double last_frame_completion_time_ms);
    
    /**
     * Calculate frame deadline with jitter limitation
     * 
     * @param frame_timestamp The timestamp of the frame
     * @param remaining_bytes_kb Remaining bytes to be transmitted in KB
     * @param last_frame_completion_time_ms Time taken to complete the last frame in ms
     * @param first_arrival_time When the first packet of this frame arrived
     * @param max_jitter_threshold Maximum acceptable jitter in ms
     * @return Expected delivery time in milliseconds
     */
    double calculateFrameDeadline(uint32_t frame_timestamp, double remaining_bytes_kb, 
                                double last_frame_completion_time_ms,
                                std::chrono::steady_clock::time_point first_arrival_time,
                                double max_jitter_threshold);
    
    // Multilink scheduling methods
    std::vector<double> computeChunkSizes(double total_size_kb, const std::vector<int>& link_ids);
    std::vector<LinkAssignment> scheduleFrame(uint32_t timestamp, double frame_size_kb, double progress);
    Json::Value getSchedulingDecisionJson(const std::vector<LinkAssignment>& assignments);

    uint32_t app_deadline_;
    uint32_t prev_frame_size_kb_ = 1;
    // Sticky last-positive bandwidth per link, used as fallback when measured=0
    // to avoid the chicken-and-egg in available_links filter.
    std::map<int, double> last_known_bw_mbps_{};

    // Jitter-adaptive deadline state (paper §5.3 eq 3 + J_on/J_off rule).
    //   J_off  = offline-measured tolerance, hardcoded at 30 ms per user spec.
    //   J_on   = online-adapted, ramps +1 ms per 500 ms tick when traffic stable;
    //            jumps to J_off if a ≥15% bitrate drop is observed within last 500 ms
    //            AND current jitter is below J_off (so the drop is NOT being caused by
    //            our own jitter — see paper §5.3 paragraph "online jitter threshold").
    double j_off_ms_ = 30.0;
    double j_on_ms_  = 0.0;
    std::chrono::steady_clock::time_point last_j_on_update_{};
    // Rolling 500-ms-window bitrate samples (kbps) for drop-detection comparison.
    double last_window_bitrate_kbps_ = 0.0;
    double prev_window_bitrate_kbps_ = 0.0;
    double last_window_jitter_ms_   = 0.0;
    // Last frame's measured arrival timestamp + completion time, for eq 3:
    //   D_m = min(D_max, T_{m-1} + (t_m - t_{m-1}) + J_th)
    // Frame_arrival_time_ms_ stores the steady_clock-derived first-packet arrival of
    // the previous frame; prev_frame_completion_ms_ is the previous frame's actual
    // delivery time. These are populated from QoEProcessor on each tick.
    int64_t prev_frame_arrival_ms_   = 0;
    double  prev_frame_completion_ms_ = 30.0;  // start at default deadline
    std::unique_ptr<AsyncLogger> j_on_logger_;
    int64_t last_path_change_ms_ = 0;  // wall ms of last committed path switch (M11 holdoff)

    // Priority-based re-injection state (paper §5.4).
    //   beta_ = configurable tolerance (default 0.1 per paper)
    //   evaluateReinjection(): inspects in-flight frames, decides whether to re-inject
    //   to the alt link given current BW/deadline. Records decisions to reinject_log_.
    // beta default 0.1 per paper; negative values fire reinject more aggressively
    // (trigger when ECT exceeds slack × (1+β), so β=-0.5 triggers at 50% of slack).
    // Override via QCON_REINJECT_BETA env. More aggressive reinject keeps the
    // backup-link CCA primed, which is necessary in our trace emulator where
    // single-link choice locks WebRTC BWE to one link and starves multilink.
    double beta_ = 0.1;
    std::unique_ptr<AsyncLogger> reinject_logger_;
    // Returns true when re-injection fires AND mutates 'decision' to point to the
    // alt link (paper §5.4 handover). False when no trigger, no alt, or ECT alt > slack.
    bool evaluateReinjection(SchedulingDecision& decision,
                             double frame_size_kb,
                             double deadline_ms,
                             const std::vector<std::pair<int, LinkMetrics>>& links);
};

} // namespace ric

#endif // QCON_SCHEDULER_HPP