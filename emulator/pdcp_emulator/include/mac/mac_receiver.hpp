#ifndef MAC_RECEIVER_HPP
#define MAC_RECEIVER_HPP

#include <memory>
#include <functional>
#include <chrono>
#include <atomic>
#include <mutex>
#include "log.h"
#include "rlc_receiver.hpp" // Include the RLC receiver module

// MAC Receiver Module - processes incoming packets from PHY layer
class MacReceiverModule {
public:
    MacReceiverModule(std::shared_ptr<RlcReceiverModule> rlcModule);
    ~MacReceiverModule();
    
    // Start the receiver module
    void start();
    
    // Stop the receiver module
    void stop();
    
    // Process received packet from PHY layer
    bool processReceivedPacket(const unsigned char* packet, size_t len);
    
    // Set callback for delivering complete packets to higher layers
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Get statistics
    uint32_t getPacketsProcessed() const;
    uint32_t getBytesProcessed() const;
    
    // Deliver reassembled packets from RLC to higher layers
    bool deliverReassembledPackets();

private:
    std::shared_ptr<RlcReceiverModule> rlc;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    std::atomic<uint32_t> packetsProcessed;
    std::atomic<uint32_t> bytesProcessed;
    std::atomic<bool> running;
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;
};

#endif // MAC_RECEIVER_HPP