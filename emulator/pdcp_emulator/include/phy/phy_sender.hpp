#ifndef PHY_SENDER_HPP
#define PHY_SENDER_HPP

#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <string>
#include <fstream>
#include "log.h"

// PHY Sender Module - Responsible for bandwidth management and transmission
class PhySenderModule {
public:
    PhySenderModule(const std::string& bwTraceFile = "", uint32_t updateIntervalMs = 100);
    ~PhySenderModule();
    
    // Start the bandwidth trace reading thread
    void start();
    
    // Stop the bandwidth trace reading thread
    void stop();
    
    // Process packet received from MAC layer
    bool processPacket(const unsigned char* packet, size_t len);
    
    // Get available bytes for transmission
    size_t getAvailableBytes(double slotDuration);
    
    // Set callback for delivering packets to the next hop
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Manually set bandwidth (useful when not using trace file)
    void setBandwidth(uint32_t bps);
    
    // Get current bandwidth
    uint32_t getCurrentBandwidth() const;
    
    // Get bytes sent
    uint32_t getBytesSent() const;

private:
    std::string bandwidthTraceFile;
    uint32_t updateInterval; // in milliseconds
    std::atomic<uint32_t> currentBandwidth; // in bits per second
    size_t accumulatedBytes; // bytes available for transmission
    std::atomic<uint32_t> bytesSent;
    
    std::thread bwUpdateThread;
    std::atomic<bool> running;
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;
    std::chrono::steady_clock::time_point lastTransmitTime;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    // Update bandwidth based on trace file
    void updateBandwidthFromTrace();
};

#endif // PHY_SENDER_HPP