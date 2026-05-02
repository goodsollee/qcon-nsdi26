#ifndef PDCP_PROCESSOR_IMPLEMENTATION_HPP
#define PDCP_PROCESSOR_IMPLEMENTATION_HPP

#include "pdcp_processor.hpp"
#include "pdcp_config.h"
#include "network/link.hpp"

#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

// Define maximum packet size
#define PDCP_MAX_PACKET_SIZE 65535

// Implementation of the PdcpProcessor interface
class PdcpProcessorImpl : public PdcpProcessor {
public:
    PdcpProcessorImpl();
    ~PdcpProcessorImpl() override;
    
    // Configuration methods
    bool configure(const PdcpConfig& config) override;
    bool configure(ConfigParser* configParser) override;
    
    // Initialization and lifecycle methods
    bool initialize() override;
    bool start() override;
    void stop() override;

    // Check if running
    bool isRunning() const override { return running.load(); }
    
    // Packet processing methods
    size_t processDownlink(unsigned char* packet, size_t len) override;
    size_t processUplink(unsigned char* packet, size_t len) override;
    
    // Register handlers
    void registerDownlinkSendHandler(int client_socket_fd) override;
    void registerUplinkSendHandler(int tun_fd) override;

private:
    // Create default configuration
    PdcpConfig createDefaultConfig();
    
    // Configuration
    PdcpConfig config;
    std::vector<PdcpConfig> linkConfigs;
    
    // Running state
    std::atomic<bool> running;
    
    // ZMQ Links
    std::unique_ptr<Link> pdcpSenderLink;  // Link to PDCP Sender
    std::unique_ptr<Link> pdcpReceiverLink; // Link from PDCP Receiver
    
    // Socket file descriptors
    int downlink_socket_fd;  // Socket to namespace
    int uplink_tun_fd;       // TUN device
    std::mutex socket_mutex; // Protect access to file descriptors
};

#endif // PDCP_PROCESSOR_IMPLEMENTATION_HPP