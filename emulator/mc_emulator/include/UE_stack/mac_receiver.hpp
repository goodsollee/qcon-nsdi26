#ifndef MAC_RECEIVER_HPP
#define MAC_RECEIVER_HPP

#include <memory>
#include <functional>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <algorithm> // For std::min_element
#include "readerwriterqueue.h" // Include moodycamel's ReaderWriterQueue
#include "log.hpp"
#include "rlc_receiver.hpp" // Include the RLC receiver module
#include "pdcp_config.h"

// Structure to hold status PDU in the queue with deadline
struct QueuedStatusPdu {
    RlcStatusPdu pdu;
    std::chrono::steady_clock::time_point queueTime;
    std::chrono::steady_clock::time_point deadlineTime;
    
    QueuedStatusPdu(const RlcStatusPdu& p, std::chrono::milliseconds delay) 
        : pdu(p), 
          queueTime(std::chrono::steady_clock::now()),
          deadlineTime(queueTime + delay) {}
};

// MAC Receiver Module - processes incoming packets from PHY layer
class MacReceiverModule {
public:
    MacReceiverModule(std::shared_ptr<RlcReceiverModule> rlcModule, const MacReceiverConfig& cfg);
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

    // Set the status PDU callback
    void setStatusPduCallback(std::function<void(const RlcStatusPdu&)> callback);

    // Queue a status PDU for transmission
    bool sendStatusPdu(const RlcStatusPdu& status_pdu);
    
    // Configure the uplink scheduling parameters
    void configureUplinkScheduling(uint32_t txOpportunityIntervalMs,
                                   uint32_t txDelayMs);
                                   
    // Trigger immediate transmission of queued packets
    bool immediateUplinkResource();

private:
    std::shared_ptr<RlcReceiverModule> rlc;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    std::atomic<uint32_t> packetsProcessed;
    std::atomic<uint32_t> bytesProcessed;
    std::atomic<bool> running;
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;

    std::function<void(const RlcStatusPdu&)> statusPduCallback;
    
    // Uplink scheduling related members
    std::chrono::milliseconds txOpportunityInterval{100}; // Default 100ms
    std::chrono::milliseconds txDelay{20}; // Default 20ms
    
    // ReaderWriterQueue for efficient producer-consumer pattern
    moodycamel::ReaderWriterQueue<QueuedStatusPdu*> txQueue{1000}; // Queue size of 1000
    std::condition_variable txCondition;
    std::mutex txMutex;
    
    // Pending PDUs that have been granted tx opportunity but waiting for deadline
    std::vector<QueuedStatusPdu*> pendingPdus;
    std::mutex pendingMutex;
    
    // Threads
    std::thread opportunityThread; // For periodic tx opportunities
    std::thread transmissionThread; // For actual transmission based on deadlines
    
    // Thread functions
    void opportunityThreadFunction();
    void transmissionThreadFunction();
    
    // Process a single transmission opportunity
    void processTxOpportunity();
    
    // Check and transmit pending PDUs that reached deadline
    void checkAndTransmitPendingPdus();
    
    // Process immediate uplink resource allocation
    bool processImmediateUplink();
    
    // Transmit a single PDU
    void transmitStatusPdu(QueuedStatusPdu* pdu);
};

#endif // MAC_RECEIVER_HPP