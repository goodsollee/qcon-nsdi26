#ifndef UE_HPP
#define UE_HPP

#include "pdcp_receiver.hpp"
#include "rlc_receiver.hpp"
#include "mac_receiver.hpp"
#include "phy_receiver.hpp"
#include "async_logger.h"
#include <memory>
#include <string>

// Configuration structure for UE
struct UEConfig {
    // Basic configuration
    std::string logFolder;
    
    // Layer specific configurations
    uint32_t pdcpReorderTimeoutMs;
    uint32_t rlcBufferSize;
    uint32_t rlcReassemblyTimeoutMs;
    
    // Default values
    UEConfig() 
        : logFolder("."),
          pdcpReorderTimeoutMs(PDCP_DEFAULT_REORDER_TIMEOUT_MS),
          rlcBufferSize(100),
          rlcReassemblyTimeoutMs(1000) // 1 second timeout
    {}
};

// UE class that integrates PDCP/RLC/MAC/PHY receiver modules
class UE {
public:
    UE(const UEConfig& cfg);
    ~UE();
    
    // Process a packet received from the network (PHY layer)
    bool processPhyPacket(const unsigned char* packet, size_t len);
    
    // Check and deliver reassembled packets up to the application
    void checkAndDeliverPackets();
    
    // Set callback for delivering packets to the application layer
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Get statistics from different layers
    void getPdcpStats(uint32_t& nextExpected, uint32_t& highestReceived, 
                     uint32_t& delivered, uint32_t& dropped) const;
    
    void getRlcStats(uint32_t& reassembled, uint32_t& dequeued, 
                    uint32_t& dropped, uint32_t& segments_processed) const;
    
    void getMacStats(uint32_t& processed_packets, uint32_t& processed_bytes) const;
    
    void getPhyStats(uint32_t& received_packets, uint32_t& received_bytes) const;
    
    // Access to module components
    std::shared_ptr<PdcpReceiver> getPdcpModule() const { return pdcpModule; }
    std::shared_ptr<RlcReceiverModule> getRlcModule() const { return rlcModule; }
    std::shared_ptr<MacReceiverModule> getMacModule() const { return macModule; }
    std::shared_ptr<PhyReceiverModule> getPhyModule() const { return phyModule; }

private:
    // Protocol stack modules
    std::shared_ptr<PdcpReceiver> pdcpModule;
    std::shared_ptr<RlcReceiverModule> rlcModule;
    std::shared_ptr<MacReceiverModule> macModule;
    std::shared_ptr<PhyReceiverModule> phyModule;
    
    // Callback when a packet is fully processed
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    // Callback for PHY to deliver to MAC
    void onPhyToMacDelivery(const unsigned char* packet, size_t len);
    
    // Callback for MAC to deliver to RLC
    void onMacToRlcDelivery(const unsigned char* packet, size_t len);
    
    // Callback for RLC to deliver to PDCP
    void onRlcToPdcpDelivery(const unsigned char* packet, size_t len);
    
    // Callback for PDCP to deliver to Application
    void onPdcpToAppDelivery(unsigned char* packet, size_t len);
    
    // UE-wide logger
    AsyncLogger logger;
};

#endif // UE_HPP