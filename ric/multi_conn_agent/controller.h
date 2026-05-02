#pragma once
#include "types.h"
#include "link_performance.h"
#include "rtp_packet_queue.h"
#include <map>
#include <mutex>
#include <memory>
#include <chrono>

namespace multiconn {

// Define callback type for link allocation updates
using LinkAllocationUpdateCallback = std::function<void(ue_id_t, frame_id_t, LinkOptimizationResult&)>;

class Controller {
public:
    Controller(std::shared_ptr<LinkPerformance> link_perf,
              std::shared_ptr<RTPPacketQueue> rtp_queue,
              LinkAllocationUpdateCallback update_callback,  // Changed this parameter
              const std::vector<LinkConfig>& link_configs);
    ~Controller() = default;

    // Main packet handling interfaces
    void handle_first_packet(ue_id_t ue_id, frame_id_t frame_id);
    void handle_marker_packet(ue_id_t ue_id, frame_id_t frame_id, uint32_t actual_frame_size);

    // RLC feedback handling
    void handle_rlc_ack(ue_id_t ue_id, frame_id_t frame_id, 
                       const RLCInfo& rlc_info);

    // Configuration updates
    void update_link_config(const LinkConfig& link_config);
    void set_frame_timeout(std::chrono::milliseconds timeout);

private:
    struct FrameState {
        LinkDecision initial_decision;
        std::vector<packet_id_t> processed_packets;
        timestamp_t creation_time;
        std::map<link_id_t, bool> link_availability;
        
        // Frame delivery tracking
        uint32_t estimated_frame_size{0};
        uint32_t actual_frame_size{0};
        uint32_t bytes_delivered{0};
        uint32_t target_delivery_time_ms{0};
        std::map<link_id_t, double> link_splits;
        bool marker_received{false};
        bool needs_reinjection{false};
    };

    // Helper methods
    void calculate_and_update_allocation(ue_id_t ue_id, frame_id_t frame_id,
                                       uint32_t size, bool is_reinjection);
    void handle_delivery_progress(ue_id_t ue_id, frame_id_t frame_id,
                                const RLCInfo& rlc_info);
    void trigger_frame_reinjection(ue_id_t ue_id, frame_id_t frame_id);
    void handle_frame_completion(ue_id_t ue_id, frame_id_t frame_id);

    // Member variables
    std::shared_ptr<LinkPerformance> link_perf_;
    std::shared_ptr<RTPPacketQueue> rtp_queue_;
    LinkAllocationUpdateCallback update_callback_;  // Store callback instead of interface
    
    mutable std::mutex controller_mutex_;
    std::map<std::pair<ue_id_t, frame_id_t>, FrameState> frame_states_;
    std::unordered_map<link_id_t, LinkConfig> link_configs_;
    
    std::chrono::milliseconds frame_timeout_{1000}; // Default 1 second
    const uint32_t DELIVERY_PROGRESS_THRESHOLD{75}; // 75%
};

} // namespace multiconn