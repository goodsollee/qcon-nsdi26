#ifndef PHY_RECEIVER_HPP
#define PHY_RECEIVER_HPP

#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <string>
#include <memory>
#include "log.h"
#include "mac_receiver.hpp" // Include MAC receiver

// PHY Receiver Module - handles reception of segmented packets
class PhyReceiverModule {
public:
    PhyReceiverModule();
    ~PhyReceiverModule();
    
    // Start the receiver module
    void start();
    
    // Stop the receiver module
    void stop();
    
    // Process received packet - forward to MAC layer
    bool processReceivedPacket(const unsigned char* packet, size_t len);
    
    // Set MAC layer
    void setMacReceiver(std::shared_ptr<MacReceiverModule> mac);
    
    // Set callback for delivering packets to the next hop
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Get statistics
    uint32_t getBytesReceived() const;
    uint32_t getPacketsReceived() const;

private:
    std::shared_ptr<MacReceiverModule> macReceiver;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    std::atomic<uint32_t> bytesReceived;
    std::atomic<uint32_t> packetsReceived;
    std::atomic<bool> running;
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;
};

#endif // PHY_RECEIVER_HPP