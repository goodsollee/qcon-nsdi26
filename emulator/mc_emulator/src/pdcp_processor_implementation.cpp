#include "../include/pdcp_processor_implementation.hpp"
#include "../utils/log.hpp"
#include "config_parser.hpp"
#include <cstring>
#include <iostream>
#include <ratio>
#include <unistd.h>
#include <netinet/in.h>

// Define module names for logging
#define PROC_MODULE "Processor"

// Implementation of PdcpProcessor factory method
std::unique_ptr<PdcpProcessor> PdcpProcessor::create() {
    return std::make_unique<PdcpProcessorImpl>();
}

// Constructor
PdcpProcessorImpl::PdcpProcessorImpl()
    : running(false),
      downlink_socket_fd(-1),
      uplink_tun_fd(-1) {
    
    // Create default configuration
    config = createDefaultConfig();
}

// Destructor
PdcpProcessorImpl::~PdcpProcessorImpl() {
    stop();
}

// Create default configuration
PdcpConfig PdcpProcessorImpl::createDefaultConfig() {
    PdcpConfig cfg;
    
    // Common settings
    cfg.common.logFoldername = "/tmp/pdcp_logs";
    cfg.common.path_num = 1;
    
    // RIC settings (disabled by default)
    cfg.ric.ric_enabled = false;
    cfg.ric.kpm_report_interval_ms = 1000;
    
    // DU link settings
    cfg.du.linkId = 0;
    cfg.du.bufferSize = 100;
    cfg.du.fixedBandwidth = 10000000; // 10 Mbps
    cfg.du.bwUpdateInterval = 100;
    cfg.du.retransmission_timeout_ms = 50;
    cfg.du.max_retransmissions = 10;
    cfg.du.poll_retransmit_timer = 25;
    
    // Receiver settings
    cfg.receiver.reordering_timeout_ms = 100;
    
    return cfg;
}

// Configure the processor
bool PdcpProcessorImpl::configure(const PdcpConfig& new_config) {
    // Can only configure if not running
    if (running.load()) {
        LOG_MODULE_ERROR(PROC_MODULE, "Cannot configure while running");
        return false;
    }
    
    config = new_config;
    return true;
}

bool PdcpProcessorImpl::configure(ConfigParser* configParser) {
    if (running.load()) {
        LOG_MODULE_ERROR(PROC_MODULE, "Cannot configure while running");
        return false;
    }
    if (!configParser) {
        LOG_MODULE_ERROR(PROC_MODULE, "Null configParser pointer");
        return false;
    }

    // Get all configurations at once
    std::vector<PdcpConfig> allConfigs = configParser->getAllPdcpConfigs();
    
    if (allConfigs.empty()) {
        LOG_MODULE_ERROR(PROC_MODULE, "No configurations retrieved from ConfigParser");
        return false;
    }
    
    // Store configurations for use by PDCP modules
    linkConfigs = allConfigs;
    
    // Use the first config (CU config) as the base for the main processor
    config = allConfigs[0];
    
    LOG_MODULE_INFO(PROC_MODULE, "Configured PDCP processor with " << allConfigs.size() << " configurations");
    LOG_MODULE_INFO(PROC_MODULE, "  Main log folder: " << config.common.logFoldername);
    LOG_MODULE_INFO(PROC_MODULE, "  Main RIC enabled: " << (config.ric.ric_enabled ? "Yes" : "No"));
    
    return true;
}

// Initialize the processor
bool PdcpProcessorImpl::initialize() {
    try {
        // Create directory for logs
        std::string cmd = "mkdir -p " + config.common.logFoldername;
        system(cmd.c_str());
        
        LOG_MODULE_INFO(PROC_MODULE, "Initializing PDCP processor");

        // Create ZMQ Links for communicating with PDCP Sender and Receiver
        
        // 1. Create Link for sending to PDCP Sender
        // This PUSH socket connects to the PDCP Sender's PULL socket (tcp://*:10000)
        pdcpSenderLink = std::make_unique<Link>(
            "PdcpProcessor_SenderLink",
            "tcp://*:0",              // PULL (bind) - ephemeral port, not used
            "tcp://127.0.0.1:10000"   // PUSH (connect) - sends to PDCP Sender
        );
        
        // 2. Create Link for receiving from PDCP Receiver
        // This PULL socket binds to receive from PDCP Receiver's PUSH socket (tcp://127.0.0.1:9000)
        pdcpReceiverLink = std::make_unique<Link>(
            "PdcpProcessor_ReceiverLink",
            "tcp://*:9000",          // PULL (bind) - receives from PDCP Receiver
            "tcp://127.0.0.1:9999"   // PUSH (connect) - not used
        );

        // Register callback for UE_ARRIVAL messages from PDCP Receiver
        pdcpReceiverLink->registerCallback("UE_ARRIVAL", [this](const Message& msg) {
            LOG_MODULE_INFO(PROC_MODULE, "Received UE_ARRIVAL from PDCP Receiver, size=" 
                << msg.payload.size());
            
            if (msg.payload.empty()) return;
            
            // Directly send to socket (no queue needed)
            unsigned char buffer[PDCP_MAX_PACKET_SIZE];
            size_t packetSize = msg.payload.size();
            
            if (packetSize > PDCP_MAX_PACKET_SIZE) {
                LOG_MODULE_ERROR(PROC_MODULE, "Packet too large for processing: " << packetSize << " bytes");
                return;
            }
            
            // Copy payload to buffer
            memcpy(buffer, msg.payload.data(), packetSize);
            
            int socket_fd;
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                socket_fd = downlink_socket_fd;
            }
            
            if (socket_fd >= 0) {
                // Send to namespace through socket
                // First send the size
                uint32_t size_n = htonl(static_cast<uint32_t>(packetSize));
                if (write(socket_fd, &size_n, sizeof(size_n)) < 0) {
                    LOG_MODULE_ERROR(PROC_MODULE, "Error writing size to socket: " << strerror(errno));
                    return;
                }
                
                // Then send the data
                if (write(socket_fd, buffer, packetSize) < 0) {
                    LOG_MODULE_ERROR(PROC_MODULE, "Error writing data to socket: " << strerror(errno));
                    return;
                }
                
                LOG_MODULE_INFO(PROC_MODULE, "Sent processed downlink packet of " << packetSize 
                    << " bytes to namespace");
            } else {
                LOG_MODULE_WARN(PROC_MODULE, "Cannot send downlink packet - socket not registered");
            }
        });

        // Start the links
        pdcpSenderLink->start();
        pdcpReceiverLink->start();
        
        LOG_MODULE_INFO(PROC_MODULE, "PDCP Processor ZMQ links initialized");
        
        return true;
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(PROC_MODULE, "Initialization error: " << e.what());
        return false;
    }
}

// Register handlers for sending packets
void PdcpProcessorImpl::registerDownlinkSendHandler(int client_socket_fd) {
    std::lock_guard<std::mutex> lock(socket_mutex);
    downlink_socket_fd = client_socket_fd;
    LOG_MODULE_INFO(PROC_MODULE, "Registered downlink send handler with socket fd: " << client_socket_fd);
}

void PdcpProcessorImpl::registerUplinkSendHandler(int tun_fd) {
    std::lock_guard<std::mutex> lock(socket_mutex);
    uplink_tun_fd = tun_fd;
    LOG_MODULE_INFO(PROC_MODULE, "Registered uplink send handler with TUN fd: " << tun_fd);
}

// Start processing
bool PdcpProcessorImpl::start() {
    if (running.load()) {
        LOG_MODULE_WARN(PROC_MODULE, "Processor already running");
        return false;
    }
    
    LOG_MODULE_INFO(PROC_MODULE, "Starting PDCP processor");
    
    running.store(true);
    
    // No threads needed since we're handling packets directly in the callbacks
    LOG_MODULE_INFO(PROC_MODULE, "PDCP processor started");
    return true;
}

// Stop processing
void PdcpProcessorImpl::stop() {
    if (!running.load()) {
        return;
    }
    
    LOG_MODULE_INFO(PROC_MODULE, "Stopping PDCP processor");
    
    running.store(false);
    
    // Stop ZMQ links
    if (pdcpSenderLink) pdcpSenderLink->stop();
    if (pdcpReceiverLink) pdcpReceiverLink->stop();
    
    LOG_MODULE_INFO(PROC_MODULE, "PDCP processor stopped");
}

// Process downlink packet (host to namespace)
size_t PdcpProcessorImpl::processDownlink(unsigned char* packet, size_t len) {
    if (!running.load()) {
        LOG_MODULE_ERROR(PROC_MODULE, "Cannot process packet - processor not running");
        return 0;
    }
    
    if (len > PDCP_MAX_PACKET_SIZE) {
        LOG_MODULE_ERROR(PROC_MODULE, "Packet too large for processing: " << len << " bytes");
        return 0;
    }
    
    LOG_MODULE_DEBUG(PROC_MODULE, "Processing downlink packet: " << len << " bytes");
    
    // Forward the packet to the PDCP Sender via ZMQ Link
    if (pdcpSenderLink) {
        // Convert packet to string for ZMQ message
        std::string payload(reinterpret_cast<char*>(packet), len);
        
        // Send as DL_DATA message to PDCP Sender
        pdcpSenderLink->sendMessage("PDCP_SENDER", "DL_DATA", payload);
        
        LOG_MODULE_DEBUG(PROC_MODULE, "Sent downlink packet of " << len << " bytes to PDCP Sender");
        
        // Return 0 to indicate the packet is being handled by the cellular stack
        return 0;
    } else {
        LOG_MODULE_WARN(PROC_MODULE, "No PDCP Sender Link available, dropping packet");
        return 0;
    }
}

// Process uplink packet (namespace to host)
size_t PdcpProcessorImpl::processUplink(unsigned char* packet, size_t len) {
    if (!running.load()) {
        LOG_MODULE_ERROR(PROC_MODULE, "Cannot process packet - processor not running");
        return 0;
    }
    
    if (len > PDCP_MAX_PACKET_SIZE) {
        LOG_MODULE_ERROR(PROC_MODULE, "Packet too large for processing: " << len << " bytes");
        return 0;
    }
    
    LOG_MODULE_DEBUG(PROC_MODULE, "Processing uplink packet: " << len << " bytes");
    
    // Send directly to TUN device
    int tun_fd;
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        tun_fd = uplink_tun_fd;
    }
    
    if (tun_fd >= 0) {
        // Write directly to TUN device
        if (write(tun_fd, packet, len) < 0) {
            LOG_MODULE_ERROR(PROC_MODULE, "Error writing to TUN: " << strerror(errno));
            return 0;
        }
        
        LOG_MODULE_INFO(PROC_MODULE, "Sent uplink packet of " << len << " bytes to TUN");
    } else {
        LOG_MODULE_WARN(PROC_MODULE, "Cannot send uplink packet - TUN fd not registered");
    }
    
    return 0;
}