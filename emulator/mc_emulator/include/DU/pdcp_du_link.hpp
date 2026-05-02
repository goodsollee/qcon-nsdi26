#ifndef PDCP_DU_LINK_HPP
#define PDCP_DU_LINK_HPP

#include <memory>
#include <string>
#include <atomic>
#include <functional>
#include <chrono>
#include <vector>
#include "pdcp_common.hpp"
#include "pdcp_config.h"
#include "pdcp_sender.hpp"
#include "rlc_sender.hpp"
#include "mac_sender.hpp"
#include "phy_sender.hpp"
#include "async_logger.hpp"
#include "RIC/ric_interface_wrapper.hpp"

class PdcpDULink : public PdcpContext {
public:
    // Constructor
    PdcpDULink(const PdcpConfig& cfg);
    
    // Destructor
    virtual ~PdcpDULink();
    
    // Process incoming packet (implementation of virtual method from PdcpContext)
    virtual size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Getters for statistics and status
    uint32_t getPacketsForwarded() const;
    double getThroughput() const;
    double getLatency() const;
    uint32_t getCurrentBandwidth() const;
    size_t getRlcBufferOccupancy() const;
    
    // Configuration methods
    void setBandwidth(uint32_t bps);
    void setBufferSize(uint32_t size);
    void setSlotDuration(uint32_t ms);
    void setBandwidthTraceFile(const std::string& filename);

    std::shared_ptr<RlcSenderModule> getRlcModule() const {
        if (rlcModules.empty()) {
            return nullptr;
        }
        return rlcModules.front();
    }
    const std::vector<std::shared_ptr<RlcSenderModule>>& getRlcModules() const { return rlcModules; }
    std::shared_ptr<MacSenderModule> getMacModule() const { return mac; }
    std::shared_ptr<PhySenderModule> getPhyModule() const { return phy; }

private:
    // Callback when PHY layer delivers a packet
    void onPhyDelivery(const unsigned char* packet, size_t len);
    
    // Link identifier
    int linkId;
    
    // Protocol stack modules
    std::vector<std::shared_ptr<RlcSenderModule>> rlcModules;
    std::vector<RlcQueueConfig> rlcQueueConfigs;
    std::shared_ptr<MacSenderModule> mac;
    std::shared_ptr<PhySenderModule> phy;
    
    // Statistics
    std::atomic<uint32_t> packetsForwarded;

    // RIC interface
    std::unique_ptr<ric::RicInterfaceWrapper> ric_interface_;
    std::thread ric_hello_thread_;
    std::atomic<bool> ric_thread_running_;
    
    // KPM metrics reporting
    std::thread kpm_metrics_thread_;
    std::atomic<bool> kpm_thread_running_;
    std::chrono::steady_clock::time_point last_kpm_report_time_;
    uint32_t kpm_report_interval_ms_;
    
    // Thread functions
    void ricHelloThread();
    void kpmMetricsThread();
    
    // RC command handler - static to match RIC_ZMQ API
    static char* handleRicCommand(const char* command_type, const char* command_params, void* user_data);
    
    // Actual command implementation
    bool processRicCommand(const char* command_type, const char* command_params);
};

#endif // PDCP_DU_LINK_HPP