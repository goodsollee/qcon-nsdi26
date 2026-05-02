#ifndef PDCP_LINK_HPP
#define PDCP_LINK_HPP

#include "pdcp_common.hpp"
#include "rlc_sender.hpp"
#include "mac_sender.hpp"
#include "phy_sender.hpp"
#include "async_logger.h"
#include "pdcp_config.h"
#include <atomic>

class PdcpLink : public PdcpContext {
public:
    PdcpLink(const PdcpConfig& cfg);
    virtual ~PdcpLink();
    
    // Process incoming packet from upper layer
    virtual size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Get statistics
    uint32_t getPacketsForwarded() const;
    double getThroughput() const;
    double getLatency() const;
    uint32_t getCurrentBandwidth() const;
    size_t getRlcBufferOccupancy() const;
    
    // Configure link parameters
    void setBandwidth(uint32_t bps);
    void setBufferSize(uint32_t size);
    void setSlotDuration(uint32_t ms);
    void setBandwidthTraceFile(const std::string& filename);

    // Access to module components
    std::shared_ptr<RlcSenderModule> getRlcModule() const { return rlc; }
    std::shared_ptr<MacSenderModule> getMacModule() const { return mac; }
    std::shared_ptr<PhySenderModule> getPhyModule() const { return phy; }
    
private:
    uint32_t linkId;
    std::atomic<uint32_t> packetsForwarded;
    
    // RLC/MAC/PHY modules
    std::shared_ptr<RlcSenderModule> rlc;
    std::shared_ptr<MacSenderModule> mac;
    std::shared_ptr<PhySenderModule> phy;
    
    // Callback for PHY to deliver packet back to PDCP
    void onPhyDelivery(const unsigned char* packet, size_t len);
};

#endif /* PDCP_LINK_HPP */