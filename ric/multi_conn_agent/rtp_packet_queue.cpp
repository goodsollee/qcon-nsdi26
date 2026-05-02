#include "rtp_packet_queue.h"
#include <iostream>


namespace multiconn {

void RTPPacketQueue::add_ue_queue(ue_id_t ue_id) {
    auto ue_queue = std::make_unique<UEQueue>();
    gettimeofday(&ue_queue->last_activity, nullptr);
    ue_queues[ue_id] = std::move(ue_queue);
    LOG_INFO(MODULE_NAME, "Created queue for UE: ", ue_id);
}

void RTPPacketQueue::process_queue() {
    while (is_running.load()) {
        PDCPPacketInfo packet;
        if (incoming_queue.pop(packet)) {
            std::lock_guard<std::mutex> lock(queues_mutex);
            
            auto it = ue_queues.find(packet.ue_id);
            if (it == ue_queues.end()) {
                LOG_DEBUG(MODULE_NAME, "UE queue not found, creating new queue for UE: ", packet.ue_id);
                add_ue_queue(packet.ue_id);
                it = ue_queues.find(packet.ue_id);
            }

            auto& pdcp_packets = it->second->pdcp_packets;
            if (pdcp_packets.size() < MAX_QUEUE_SIZE) {
                pdcp_packets.push_back(std::move(packet));  // Changed from push to push_back
                gettimeofday(&it->second->last_activity, nullptr);
                
                LOG_DEBUG(MODULE_NAME, "Queued PDCP packet - UE: ", packet.ue_id,
                         ", PDCP SN: ", packet.pdcp_sn,
                         ", Frame ID: ", packet.frame_id,
                         ", Size: ", packet.data.size(), " bytes",
                         ", Queue size: ", pdcp_packets.size());
            } else {
                LOG_WARNING(MODULE_NAME, "Queue full for UE: ", packet.ue_id,
                          ", dropping packet - PDCP SN: ", packet.pdcp_sn);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

void RTPPacketQueue::queue_pdcp_packet(const PDCPPacketInfo& packet) {
    LOG_DEBUG(MODULE_NAME, "PDCP packet queued");
    incoming_queue.push(std::move(packet));
}

void RTPPacketQueue::update_rlc_info(const RLCInfo& rlc_info) {
    std::lock_guard<std::mutex> lock(queues_mutex);
    
    auto it = ue_queues.find(rlc_info.ue_id);
    if (it == ue_queues.end()) {
        LOG_WARNING(MODULE_NAME, "Queue not found for UE: ", rlc_info.ue_id);
        return;
    }

    auto& pdcp_packets = it->second->pdcp_packets;
    for (auto& packet : pdcp_packets) {
        if (packet.pdcp_sn == rlc_info.pdcp_sn) {
            // Add the single RLC SN and its info to the packet's map
            packet.rlc_info[rlc_info.rlc_sn] = rlc_info;
            
            LOG_DEBUG(MODULE_NAME, "Updated RLC info - UE: ", rlc_info.ue_id,
                     ", PDCP SN: ", rlc_info.pdcp_sn,
                     ", Link: ", rlc_info.link_id,
                     ", RLC SN: ", rlc_info.rlc_sn,
                     ", Total RLC SNs: ", packet.rlc_info.size());

            // Update link performance information
            link_perf_->update_rlc_info(rlc_info, 0);
            return;
        }
    }

    LOG_WARNING(MODULE_NAME, "PDCP packet not found - UE: ", rlc_info.ue_id,
                ", PDCP SN: ", rlc_info.pdcp_sn);
}

void RTPPacketQueue::update_rlc_ack(const RLCInfo& rlc_info) {
    std::lock_guard<std::mutex> lock(queues_mutex);

    auto it = ue_queues.find(rlc_info.ue_id);
    if (it == ue_queues.end()) return;

    auto& pdcp_packets = it->second->pdcp_packets;
    frame_id_t current_frame = 0;
    bool frame_complete = false;

    for (auto packet_it = pdcp_packets.begin(); packet_it != pdcp_packets.end();) {
        auto& rlc_info_map = packet_it->rlc_info;
        
        // Find and update RLCInfo for the acknowledged RLC SN
        auto rlc_it = rlc_info_map.find(rlc_info.rlc_sn);
        if (rlc_it != rlc_info_map.end()) {
            // Update or remove the RLC info

            // Convert timestamp difference to milliseconds
            uint64_t time_diff_us = (rlc_info.timestamp - rlc_it->second.timestamp);  

            link_perf_->update_rlc_info(rlc_info, time_diff_us);

            rlc_info_map.erase(rlc_it);
            
            LOG_DEBUG(MODULE_NAME, "RLC SN acknowledged - UE: ", rlc_info.ue_id,
                     ", PDCP SN: ", packet_it->pdcp_sn,
                     ", RLC SN: ", rlc_info.rlc_sn,
                     ", Remaining RLC SNs: ", rlc_info_map.size());

            // If packet has no more pending RLC SNs, remove it
            if (rlc_info_map.empty()) {
                current_frame = packet_it->frame_id;
                bool was_marker = packet_it->is_marker;
                
                LOG_DEBUG(MODULE_NAME, "Packet fully acknowledged - UE: ", rlc_info.ue_id,
                         ", PDCP SN: ", packet_it->pdcp_sn,
                         ", Frame: ", current_frame);

                packet_it = pdcp_packets.erase(packet_it);

                // If this was a marker packet and we've removed it, frame is complete
                if (was_marker) {
                    frame_complete = true;
                    if (frame_completion_callback_) {
                        LOG_INFO(MODULE_NAME, "Frame completed - UE: ", rlc_info.ue_id,
                                ", Frame: ", current_frame);
                        frame_completion_callback_(rlc_info.ue_id, current_frame);
                    }
                }
                continue;
            }
        }
        ++packet_it;
    }
}

std::vector<PDCPPacketInfo> RTPPacketQueue::get_frame_pdcp_packets(ue_id_t ue_id, frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(queues_mutex);
    std::vector<PDCPPacketInfo> frame_packets;
    
    auto it = ue_queues.find(ue_id);
    if (it == ue_queues.end() || it->second->pdcp_packets.empty()) {
        LOG_DEBUG(MODULE_NAME, "No packets found for UE ", ue_id,
                 ", Frame ID: ", frame_id);
        return frame_packets;
    }

    auto& pdcp_packets = it->second->pdcp_packets;
    bool find_frame_packets = false;
    for(const auto& packet : pdcp_packets) {
        if (packet.frame_id == frame_id) {
            frame_packets.push_back(packet);
            LOG_DEBUG(MODULE_NAME, "Found packet for Frame ", frame_id,
                     " - UE: ", ue_id,
                     ", PDCP SN: ", packet.pdcp_sn);
            find_frame_packets = true;
        }
        else if (packet.frame_id != frame_id && find_frame_packets) {
            break;  // Stop searching as we've found all packets for this frame
        }
    }
    
    LOG_DEBUG(MODULE_NAME, "Retrieved ", frame_packets.size(),
             " packets for Frame ", frame_id,
             ", UE ", ue_id);
    
    return frame_packets;
}

void RTPPacketQueue::remove_ue_queue(ue_id_t ue_id) {
    std::lock_guard<std::mutex> lock(queues_mutex);
    auto result = ue_queues.erase(ue_id);
    if (result > 0) {
        LOG_INFO(MODULE_NAME, "Removed queue for UE: ", ue_id);
    } else {
        LOG_WARNING(MODULE_NAME, "Queue not found for UE: ", ue_id);
    }
}

void RTPPacketQueue::cleanup_old_queues(int timeout_seconds) {
    std::lock_guard<std::mutex> lock(queues_mutex);
    timeval now;
    gettimeofday(&now, nullptr);

    auto it = ue_queues.begin();
    size_t removed_count = 0;
    
    while (it != ue_queues.end()) {
        if ((now.tv_sec - it->second->last_activity.tv_sec) > timeout_seconds) {
            LOG_INFO(MODULE_NAME, "Removing inactive queue for UE: ", it->first,
                    ", Inactive time: ", (now.tv_sec - it->second->last_activity.tv_sec), "s");
            it = ue_queues.erase(it);
            removed_count++;
        } else {
            ++it;
        }
    }
    
    if (removed_count > 0) {
        LOG_INFO(MODULE_NAME, "Cleanup completed - Removed ", removed_count, " inactive queues");
    }
}

}  // namespace multiconn