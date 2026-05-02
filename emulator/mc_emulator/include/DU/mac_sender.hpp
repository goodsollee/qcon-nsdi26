#ifndef MAC_SENDER_HPP
#define MAC_SENDER_HPP

#include "rlc_sender.hpp"
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <vector>
#include <cstdint>
#include "log.hpp"

// MAC Sender Module - Responsible for slot-based scheduling
class MacSenderModule {
public:
    struct QueueHandle {
        uint8_t queueId = 0;
        uint8_t priority = 0;
        std::shared_ptr<RlcSenderModule> module;
    };

    MacSenderModule(std::vector<QueueHandle> rlcQueues, double slotDurationMs = 1);
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
    
    // Get current throughput (bytes/second)
    double getThroughputBps() const;
    
    // Get packet latency (ms)
    double getAverageLatency() const;

    // For RIC integration - trigger immediate scheduling
    void triggerImmediateScheduling() {
        // Implementation depends on your MAC module's design
        // This could be a flag that's checked in your MAC thread loop
        std::lock_guard<std::mutex> lock(schedule_mutex);
        force_immediate_schedule = true;
        schedule_cv.notify_one();  // Notify the MAC thread if it's waiting
    }

private:
    std::vector<QueueHandle> rlcQueues;
    std::function<bool(const unsigned char*, size_t)> phyCallback;
    std::function<size_t(double)> getAvailableBytesCallback;
    
    std::thread schedulerThread;
    std::atomic<bool> running;
    std::condition_variable cv;
    std::mutex mutex;


    // Add these if not already present
    std::mutex schedule_mutex;
    std::condition_variable schedule_cv;
    bool force_immediate_schedule = false;
    
    double slotDuration; // in milliseconds
    std::atomic<uint32_t> packetsSent;
    std::atomic<uint64_t> totalLatency; // in milliseconds
    std::atomic<uint64_t> bytesSent;    // total bytes sent
    
    std::chrono::steady_clock::time_point lastSlotTime;
    std::chrono::steady_clock::time_point lastThroughputCalcTime;
    std::atomic<double> currentThroughputBps; // Current throughput in bytes per second
    
    // Scheduler function that runs in a separate thread
    void schedulerFunction();
    
    // Process all packets that can be sent in the current slot
    void processSlot();
};

#endif // MAC_SENDER_HPP