#pragma once
#include "types.h"
#include "rtp_packet_queue.h"
#include "link_performance.h"
#include "controller.h"
#include <memory>

namespace multiconn {

// Forward declarations - must be inside namespace
class Controller;  // This must be before the ControlInterface class

// Define callback function types
using LinkAllocationCallback = std::function<void(const RICControlMessage&)>;
using PacketForwardCallback = std::function<void(const RICControlMessage&)>;

class ControlInterface : public std::enable_shared_from_this<ControlInterface> {
public:
    explicit ControlInterface(LinkAllocationCallback link_allocation_cb, PacketForwardCallback packet_forward_cb);
    ~ControlInterface() = default;

    // RAN information processing
    void process_pdcp_packet(const PDCPPacketInfo& packet);
    void process_rlc_update(const RLCInfo& rlc_info);
    void process_delay_update(const DelayInfo& delay_info);
    void process_rlc_ack(const RLCInfo& rlc_info);
    
    // Packet arrival handling
    void handle_frame_arrival(ue_id_t ue_id, frame_id_t frame_id, size_t size, bool is_completed);

    // Link configuration
    // void update_link_config(const LinkConfig& link_config);
    // std::vector<LinkConfig> get_active_link_configs() const;
    
    // Configuration methods
    void set_metric_timeout(std::chrono::milliseconds timeout);
    void set_throughput_window(std::chrono::milliseconds window);
    void set_frame_timeout(std::chrono::milliseconds timeout);

     // Forward PDCP packets to RIC for transmission
    void forward_pdcp_packets(ue_id_t ue_id, const std::vector<PDCPPacketInfo>& packets);

    // Control information delivery
    void update_link_allocation(ue_id_t ue_id, frame_id_t frame_id, LinkOptimizationResult& result);

    // Packet delivery
    void send_packets_to_ric(ue_id_t ue_id, const std::vector<PDCPPacketInfo>& packets);

    //TODO packet and control information delivery

private:
    std::shared_ptr<RTPPacketQueue> rtp_packet_queue_;
    std::shared_ptr<LinkPerformance> link_perf_;
    std::unique_ptr<Controller> controller_;
    std::vector<LinkConfig> link_configs_;

    // Callbacks for RIC communication
    LinkAllocationCallback link_allocation_callback_;
    PacketForwardCallback packet_forward_callback_;
};

} // namespace multiconn