#ifndef MAC_SENDER_HPP
#define MAC_SENDER_HPP

#include "rlc_sender.hpp"
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <chrono>
#include "log.h"

// MAC Sender Module - Responsible for slot-based scheduling
class MacSenderModule {
public:
    MacSenderModule(std::shared_ptr<RlcSenderModule> rlcModule, double slotDurationMs = 1); 
    ~MacSenderModule();
    
    // Start the scheduling thread
    void start();
    
    // Stop the scheduling thread
    void stop();
    
    // Set callback for passing packets to PHY layer
    void setPhyCallback(std::function<bool(const unsigned char*, size_t)> callback);
    
    // Set callback for getting available bytes from PHY layer
    void setGetAvailableBytesCallback(std::function<size_t(double)> callback);
    
    // Get current throughput (packets/second)
    double getThroughput() const;
    
    // Get packet latency (ms)
    double getAverageLatency() const;

private:
    std::shared_ptr<RlcSenderModule> rlc;
    std::function<bool(const unsigned char*, size_t)> phyCallback;
    std::function<size_t(double)> getAvailableBytesCallback;
    
    std::thread schedulerThread;
    std::atomic<bool> running;
    std::condition_variable cv;
    std::mutex mutex;
    
    double slotDuration; // in milliseconds
    std::atomic<uint32_t> packetsSent;
    std::atomic<uint64_t> totalLatency; // in milliseconds
    
    std::chrono::steady_clock::time_point lastSlotTime;
    
    // Scheduler function that runs in a separate thread
    void schedulerFunction();
    
    // Process all packets that can be sent in the current slot
    void processSlot();
};

#endif // MAC_SENDER_HPP