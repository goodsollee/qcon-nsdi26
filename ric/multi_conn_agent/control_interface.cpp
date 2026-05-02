#include "control_interface.h"
#include "controller.h"  // Include the full definition here
#include "../utils/config_reader.h"

namespace multiconn {

ControlInterface::ControlInterface(LinkAllocationCallback link_allocation_cb,
                                    PacketForwardCallback packet_forward_cb)
    : link_allocation_callback_(link_allocation_cb),
      packet_forward_callback_(packet_forward_cb) {
    
    // Load link configurations
    std::vector<LinkConfig> link_configs = 
        ConfigReader::readLinkConfigs("../configs/link_config.csv");

    link_perf_ = std::make_shared<LinkPerformance>(link_configs);
    rtp_packet_queue_ = std::make_shared<RTPPacketQueue>(link_perf_);

    // Create controller with direct callback
    auto update_callback = [this](ue_id_t ue_id, frame_id_t frame_id, 
                                LinkOptimizationResult& result) {
        this->update_link_allocation(ue_id, frame_id, result);
    };

    controller_ = std::make_unique<Controller>(
        link_perf_,
        rtp_packet_queue_,
        update_callback,
        link_configs
    );

    LOG_INFO(MODULE_NAME, "Control interface initialized with ", link_configs.size(), " links");
    for (const auto& config : link_configs) {
        LOG_INFO(MODULE_NAME, "Link configured - ID: ", config.link_id,
                 ", Name: ", config.link_name,
                 ", Priority: ", config.priority);
    }
}

void ControlInterface::process_pdcp_packet(const PDCPPacketInfo& packet) {
    LOG_DEBUG(MODULE_NAME, "Processing PDCP packet - UE: ", packet.ue_id,
             ", Frame: ", packet.frame_id,
             ", PDCP sn: ", packet.pdcp_sn);
    
    rtp_packet_queue_->queue_pdcp_packet(packet);
}

void ControlInterface::process_rlc_update(const RLCInfo& rlc_info) {
    LOG_DEBUG(MODULE_NAME, "Processing RLC info - UE: ", rlc_info.ue_id,
             ", Link: ", rlc_info.link_id,
             ", Is ACK: ", rlc_info.is_ack);
    
    rtp_packet_queue_->update_rlc_info(rlc_info);
}

void ControlInterface::process_delay_update(const DelayInfo& delay_info) {
    LOG_DEBUG(MODULE_NAME, "Processing delay update - UE: ", delay_info.ue_id,
             ", Link: ", delay_info.link_id,
             ", Xn delay: ", delay_info.xn_delay_ms, "ms");
    
    link_perf_->update_delay_info(delay_info);
}

void ControlInterface::process_rlc_ack(const RLCInfo& rlc_info) {
    LOG_DEBUG(MODULE_NAME, "Processing RLC ACK - UE: ", rlc_info.ue_id,
             ", Link: ", rlc_info.link_id,
             ", Is ACK: ", rlc_info.is_ack);
    
    rtp_packet_queue_->update_rlc_ack(rlc_info);
}

void ControlInterface::handle_frame_arrival(ue_id_t ue_id, frame_id_t frame_id, size_t size, bool is_completed) {
    
    LOG_DEBUG(MODULE_NAME, "Handling frame arrival - UE: ", ue_id,
             ", Frame: ", frame_id);
    
    if (!is_completed) {
        controller_->handle_first_packet(ue_id, frame_id);
    } else {
        controller_->handle_marker_packet(ue_id, frame_id, size);
    }
}

void ControlInterface::forward_pdcp_packets(ue_id_t ue_id, 
                                          const std::vector<PDCPPacketInfo>& packets) {
    // Create RIC message for packet forwarding
    RICControlMessage control_msg;
    control_msg.ue_id = ue_id;
    control_msg.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    control_msg.message_type = RICMessageType::PDCP_PACKETS;
    
    // Add packet data for GTP-U tunnel
    for (const auto& packet : packets) {
        RICPacketData pkt_data;
        pkt_data.pdcp_sn = packet.pdcp_sn;
        pkt_data.data = packet.data;
        control_msg.pdcp_packets.push_back(pkt_data);
    }
    
    try {
        // Use callback instead of direct RIC interface
        if (packet_forward_callback_) {
            packet_forward_callback_(control_msg);
            LOG_INFO(MODULE_NAME, "Forwarded PDCP packets via callback - UE: ", ue_id,
                     ", Packets: ", packets.size());
        } else {
            LOG_ERROR(MODULE_NAME, "Packet forward callback not set");
        }
    } catch (const std::exception& e) {
        LOG_ERROR(MODULE_NAME, "Failed to forward packets: ", e.what());
    }
}

void ControlInterface::update_link_allocation(ue_id_t ue_id, frame_id_t frame_id, LinkOptimizationResult& result) {
    if (result.selected_links.empty()) {
        LOG_WARNING(MODULE_NAME, "Invalid link optimization result - UE: ", ue_id,
                   ", Frame: ", frame_id);
        return;
    }

    // Create control message for RIC
    RICControlMessage control_msg;
    control_msg.ue_id = ue_id;
    control_msg.frame_id = frame_id;
    control_msg.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    
    // Add link allocation information
    for (size_t i = 0; i < result.selected_links.size(); i++) {
        RICLinkControl link_control;
        link_control.link_id = result.selected_links[i];
        link_control.allocation_ratio = result.split_ratios[i];
        control_msg.link_controls.push_back(link_control);
    }

    // Send to RIC
    try {
        // Use callback instead of direct RIC interface
        if (link_allocation_callback_) {
            link_allocation_callback_(control_msg);
            LOG_INFO(MODULE_NAME, "Sent link allocation via callback - UE: ", ue_id,
                     ", Frame: ", frame_id,
                     ", Links: ", result.selected_links.size(),
                     ", Target delivery time: ", result.total_delivery_time_ms, "ms");
        } else {
            LOG_ERROR(MODULE_NAME, "Link allocation callback not set");
        }
    } catch (const std::exception& e) {
        LOG_ERROR(MODULE_NAME, "Failed to send link allocation: ", e.what());
    }
}

} // namespace multiconn