#include "controller.h"
#include "control_interface.h"
#include <algorithm>
#include <numeric>
#include <sys/time.h>

// Workflow
// 1. When the first packet of frame arrives, determine which links to use and their split ratios according to the estimate size. (action: split ratio)
// 2. When RLC ACK arrives, check whether progress would not violate the deadline. If violate, reinject the frame to unused link. (action: reinject)
// 3. When the marker packet of frame arrives, check whether the frame would be delivered on time. If not, reinject the frame to unused link. (action: reinject)

namespace multiconn {

// In controller.cpp
Controller::Controller(std::shared_ptr<LinkPerformance> link_perf,
                     std::shared_ptr<RTPPacketQueue> rtp_queue,
                     LinkAllocationUpdateCallback update_callback,
                     const std::vector<LinkConfig>& link_configs)
    : link_perf_(link_perf)
    , rtp_queue_(rtp_queue)
    , update_callback_(std::move(update_callback)) {
    
    for (const auto& config : link_configs) {
        link_configs_[config.link_id] = config;
        LOG_INFO(MODULE_NAME, "Controller initialized link: ", config.link_id,
                ", Name: ", config.link_name);
    }

    // Set frame completion callback
    rtp_queue_->set_frame_completion_callback(
        [this](ue_id_t ue_id, frame_id_t frame_id) {
            this->handle_frame_completion(ue_id, frame_id);
        }
    );
}

void Controller::handle_first_packet(ue_id_t ue_id, frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(controller_mutex_);

    // TODO w/ bitrate
    uint32_t estimated_size = 1000;

    LOG_DEBUG(MODULE_NAME, "Handling first packet - UE: ", ue_id,
             ", Frame: ", frame_id,
             ", Estimated size: ", estimated_size);

    calculate_and_update_allocation(ue_id, frame_id, estimated_size, false);
}

void Controller::handle_rlc_ack(ue_id_t ue_id, frame_id_t frame_id, 
                              const RLCInfo& rlc_info) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    
    auto frame_it = frame_states_.find({ue_id, frame_id});
    if (frame_it == frame_states_.end()) {
        LOG_WARNING(MODULE_NAME, "No frame state found for RLC ACK - UE: ", ue_id,
                   ", Frame: ", frame_id);
        return;
    }

    auto& frame_state = frame_it->second;
    
    // Update delivered bytes - using single acked_bytes value
    frame_state.bytes_delivered += rlc_info.acked_bytes;

    handle_delivery_progress(ue_id, frame_id, rlc_info);
}

void Controller::handle_marker_packet(ue_id_t ue_id, frame_id_t frame_id, 
                                    uint32_t actual_frame_size) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    
    auto frame_it = frame_states_.find({ue_id, frame_id});
    if (frame_it == frame_states_.end()) {
        LOG_WARNING(MODULE_NAME, "No frame state found for marker packet - UE: ", ue_id,
                   ", Frame: ", frame_id);
        return;
    }

    auto& frame_state = frame_it->second;
    frame_state.actual_frame_size = actual_frame_size;
    frame_state.marker_received = true;

    // Recalculate allocation if actual size is significantly different
    /*
    if (std::abs(static_cast<int>(actual_frame_size) - 
                static_cast<int>(frame_state.estimated_frame_size)) > 1024) {
        calculate_and_update_allocation(ue_id, frame_id, actual_frame_size, false);
    }
    */
}

void Controller::calculate_and_update_allocation(ue_id_t ue_id, frame_id_t frame_id,
                                              uint32_t size, bool is_reinjection) {
    auto optimization_result = link_perf_->calculate_delivery_time(ue_id, size);
    
    auto& frame_state = frame_states_[{ue_id, frame_id}];
    if (!is_reinjection) {
        frame_state.creation_time = std::chrono::steady_clock::now().time_since_epoch().count();
        frame_state.estimated_frame_size = size;
    }
    frame_state.target_delivery_time_ms = optimization_result.total_delivery_time_ms;
    
    // Update link splits and availability
    frame_state.link_splits.clear();
    for (size_t i = 0; i < optimization_result.selected_links.size(); i++) {
        link_id_t link_id = optimization_result.selected_links[i];
        frame_state.link_splits[link_id] = optimization_result.split_ratios[i];
        frame_state.link_availability[link_id] = true;
    }

    // Use callback directly instead of interface
    if (update_callback_) {
        update_callback_(ue_id, frame_id, optimization_result);
    } else {
        LOG_ERROR(MODULE_NAME, "Link allocation update callback not set");
    }
}

void Controller::handle_delivery_progress(ue_id_t ue_id, frame_id_t frame_id,
                                        const RLCInfo& rlc_info) {
    auto& frame_state = frame_states_[{ue_id, frame_id}];
    
    // Calculate completion percentage
    double completion_pct = static_cast<double>(frame_state.bytes_delivered) / 
                          frame_state.estimated_frame_size * 100.0;

    uint64_t elapsed_time = (std::chrono::steady_clock::now().time_since_epoch().count() - 
                           frame_state.creation_time) / 1000000; // Convert to ms

    double expected_completion = static_cast<double>(elapsed_time) / 
                               frame_state.target_delivery_time_ms * 100.0;

    if (completion_pct < expected_completion * 0.75) { // 25% behind schedule
        LOG_WARNING(MODULE_NAME, "Frame delivery significantly behind schedule - UE: ", ue_id,
                   ", Frame: ", frame_id,
                   ", Completion: ", completion_pct, "%",
                   ", Expected: ", expected_completion, "%");

        trigger_frame_reinjection(ue_id, frame_id);
    }
}

void Controller::trigger_frame_reinjection(ue_id_t ue_id, frame_id_t frame_id) {
    auto& frame_state = frame_states_[{ue_id, frame_id}];
    
    if (frame_state.needs_reinjection) {
        return; // Already being reinjected
    }

    // Get frame packets from RTP queue
    auto frame_packets = rtp_queue_->get_frame_pdcp_packets(ue_id, frame_id);
    uint32_t remaining_size = 0;
    
    // Calculate remaining size from unacknowledged packets
    for (const auto& packet : frame_packets) {
        remaining_size += packet.data.size();
    }

    if (remaining_size > 0) {
        frame_state.needs_reinjection = true;
        calculate_and_update_allocation(ue_id, frame_id, remaining_size, true);
    }
}

void Controller::handle_frame_completion(ue_id_t ue_id, frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    
    auto frame_it = frame_states_.find({ue_id, frame_id});
    if (frame_it != frame_states_.end()) {
        auto& frame_state = frame_it->second;
        
        // Log completion metrics
        uint64_t elapsed_time = (std::chrono::steady_clock::now().time_since_epoch().count() - 
                                frame_state.creation_time) / 1000000;
        
        LOG_INFO(MODULE_NAME, "Frame delivery completed - UE: ", ue_id,
                    ", Frame: ", frame_id,
                    ", Total time: ", elapsed_time, "ms",
                    ", Target time: ", frame_state.target_delivery_time_ms, "ms",
                    ", Total size: ", frame_state.estimated_frame_size, " bytes");

        // Remove frame state
        frame_states_.erase(frame_it);
        LOG_DEBUG(MODULE_NAME, "Cleaned up completed frame state - UE: ", ue_id,
                    ", Frame: ", frame_id);
    }
}

void Controller::update_link_config(const LinkConfig& link_config) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    link_configs_[link_config.link_id] = link_config;
    LOG_INFO(MODULE_NAME, "Updated link configuration - Link: ", link_config.link_id,
             ", Name: ", link_config.link_name);
}

void Controller::set_frame_timeout(std::chrono::milliseconds timeout) {
    frame_timeout_ = timeout;
}

} // namespace multiconn