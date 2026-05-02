#ifndef PDCP_UE_LINK_HPP
#define PDCP_UE_LINK_HPP

#include "pdcp_common.hpp"
#include "rlc_receiver.hpp"
#include "mac_receiver.hpp"
#include "phy_receiver.hpp"
#include "async_logger.hpp"
#include <memory>
#include <string>
#include <functional>
#include <atomic>


// PdcpUELink class that integrates PDCP/RLC/MAC/PHY receiver modules
class PdcpUELink : public PdcpContext {
public:
    PdcpUELink(const PdcpConfig& cfg);
    ~PdcpUELink();
    
    // Process a packet (implementing PdcpContext interface)
    size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Process a packet received at PHY layer
    bool processPhyPacket(const unsigned char* packet, size_t len);
    
    // Check and deliver reassembled packets
    void checkAndDeliverPackets();
    
    // Set callback for delivering packets to the application layer
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);

    // Set multi-queue aware callback for delivering packets with queue information
    void setMultiQueueDeliveryCallback(std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> callback);
    
    void getRlcStats(uint32_t& reassembled, uint32_t& dequeued, 
                    uint32_t& dropped, uint32_t& segments_processed) const;
    
    void getMacStats(uint32_t& processed_packets, uint32_t& processed_bytes) const;
    
    void getPhyStats(uint32_t& received_packets, uint32_t& received_bytes) const;
    
    // Access to module components
    std::shared_ptr<RlcReceiverModule> getRlcModule() const { return rlcModule; }
    std::shared_ptr<MacReceiverModule> getMacModule() const { return macModule; }
    std::shared_ptr<PhyReceiverModule> getPhyModule() const { return phyModule; }

private:
    // Protocol stack modules
    std::shared_ptr<RlcReceiverModule> rlcModule;
    std::shared_ptr<MacReceiverModule> macModule;
    std::shared_ptr<PhyReceiverModule> phyModule;
    
    // Callback when a packet is fully processed
    std::function<void(const unsigned char*, size_t)> deliveryCallback;

    // Multi-queue aware callback for PDCP delivery
    std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> multiQueueDeliveryCallback;
    
    // Callback for PHY to deliver to MAC
    void onPhyToMacDelivery(const unsigned char* packet, size_t len);
    
    // Callback for MAC to deliver to RLC
    void onMacToRlcDelivery(const unsigned char* packet, size_t len);
    
    // Callback for RLC to deliver to PDCP
    void onRlcToPdcpDelivery(const unsigned char* packet, size_t len);

    // Multi-queue aware callback for RLC to deliver to PDCP with queue information
    void onRlcToPdcpMultiQueueDelivery(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority);
    
    // UE-wide logger
    AsyncLogger logger;
};

#endif // PDCP_UE_LINK_HPP