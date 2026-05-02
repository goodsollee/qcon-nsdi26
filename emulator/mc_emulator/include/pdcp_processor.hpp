#ifndef PDCP_PROCESSOR_HPP
#define PDCP_PROCESSOR_HPP

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

// Forward declarations
struct PdcpConfig;

class ConfigParser; // Forward declaration

// PDCP Processor interface
class PdcpProcessor {
public:
    // Virtual destructor
    virtual ~PdcpProcessor() {}
    
    // Initialize the processor
    virtual bool initialize() = 0;
    
    // Start processing
    virtual bool start() = 0;
    
    // Stop processing
    virtual void stop() = 0;

    // Process downlink packet (host to namespace)
    // Returns 0 if packet is handled internally by the cellular stack
    // Returns > 0 if the packet should be forwarded immediately
    virtual size_t processDownlink(unsigned char* packet, size_t len) = 0;
    
    // Process uplink packet (namespace to host)
    // Returns 0 if packet is handled internally by the cellular stack
    // Returns > 0 if the packet should be forwarded immediately
    virtual size_t processUplink(unsigned char* packet, size_t len) = 0;
    
    // Register send packet handler for downlink (cellular stack to namespace)
    virtual void registerDownlinkSendHandler(int client_socket_fd) = 0;
    
    // Register send packet handler for uplink (cellular stack to host)
    virtual void registerUplinkSendHandler(int tun_fd) = 0;
    
    // Configure the processor
    virtual bool configure(const PdcpConfig& config) = 0;

    // [NEW] Overload or alternative to pass the parser directly
    virtual bool configure(ConfigParser* configParser) = 0;
    
    // Check if running
    virtual bool isRunning() const = 0;
    
    // Factory method to create an instance
    static std::unique_ptr<PdcpProcessor> create();
};

#endif // PDCP_PROCESSOR_HPP