#ifndef PDCP_RECEIVER_HPP
#define PDCP_RECEIVER_HPP

#include "pdcp_common.hpp"
#include "async_logger.h"
#include "pdcp_config.h"
#include <mutex>
#include <atomic>
#include <vector>
#include <thread>
#include <condition_variable>
#include <functional>  // <<== Add this

#include "readerwriterqueue.h"

// Maximum reordering window size
#define PDCP_MAX_REORDER_WINDOW 1000
// Default reordering timeout in milliseconds
#define PDCP_DEFAULT_REORDER_TIMEOUT_MS 300

// Structure to hold a packet in the reordering buffer
struct PdcpPacket {
    std::vector<unsigned char> data;
    uint32_t  epoch;
    uint32_t sequenceNumber;
    struct timespec timestamp;
    bool inUse;

    uint64_t pdcpSentTime;       // store the sender's "send_timestamp" in ms
    uint64_t arrivalTime;        // store the receiver arrival time in ms
    
    PdcpPacket() : sequenceNumber(0), inUse(false) {
        data.reserve(PDCP_MAX_PACKET_SIZE);
        timestamp = {0, 0};
    }
};

class PdcpReceiver : public PdcpContext {
public:
    PdcpReceiver(const PdcpConfig& cfg);
    virtual ~PdcpReceiver();
    
    // Process a packet (extract sequence and reorder)
    virtual size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Deliver a packet from the reordering buffer
    // Returns true if a packet was delivered, false otherwise
    bool deliverPacket(unsigned char* packetOut, size_t& lenOut);
    
    // Get statistics
    void getStats(uint32_t& nextExpected, uint32_t& highestReceived, 
                  uint32_t& deliveredCount, uint32_t& droppedCount) const;


    // Define a callback that takes no arguments.
    using DeliveryCallback = std::function<void()>;

    void setDeliveryCallback(DeliveryCallback callback) {
        deliveryCallback = callback;
    }

private:
    DeliveryCallback deliveryCallback;

    // Timer thread function
    void timerThreadFunc();
    
    // Check and handle timeout
    void checkReorderingTimeout();

    // Start global reordering timer
    void startReorderingTimer();

    void stopReorderingTimer();

    // Reordering buffer
    std::vector<PdcpPacket> packets;
    std::vector<bool> packetPresent;
    
    // Sequence counters
    std::atomic<uint32_t> nextExpected;    // Next sequence number expected
    std::atomic<uint32_t> highestReceived; // Highest sequence received
    std::atomic<uint32_t> previousEpoch;    // Highest epoch received
    std::atomic<uint32_t> highestEpoch;    // Highest epoch received
    std::atomic<bool> timerRunning;        // Whether timer is running
    
    // Reordering timeout value
    int reorderTimeoutMs;
    
    // Statistics
    std::atomic<uint32_t> deliveredCount;
    std::atomic<uint32_t> droppedCount;
    
    // Mutex for buffer access
    std::mutex bufferMutex;
    
    // Timer related members
    std::thread timerThread;
    std::condition_variable timerCV;
    std::mutex timerMutex;
    bool stopThread;
    struct timespec lastGapTime;        // Timestamp when a gap was detected

    AsyncLogger async_logger;
};

#endif /* PDCP_RECEIVER_HPP */