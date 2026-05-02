#pragma once

#include <map>
#include <chrono>
#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>
#include <functional>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <set>
#include <arpa/inet.h>
#include "readerwriterqueue.h"
#include "log.hpp"
#include "rlc_retransmission.hpp" // For RlcStatusPdu definition
#include "rlc_segmentation.hpp" // For RlcSegment definition
#include "rlc_sender.hpp"
#include "pdcp_config.h"
#include "async_logger.hpp" // For AsyncLogger

// Constants
#define RLC_MAX_REASSEMBLY_CONTEXTS 100  // Maximum concurrent reassembly contexts

// Structure for reassembly context
struct ReassemblyContext {
    uint32_t original_size;
    uint16_t total_segments;
    uint16_t segments_received;
    std::vector<unsigned char> data;
    std::vector<bool> segment_received;
    std::chrono::steady_clock::time_point last_update;
};

// RLC Reassembly Manager class
class RlcReassemblyManager {
public:
    RlcReassemblyManager(uint32_t timeout_ms = 2000);
    ~RlcReassemblyManager();
    
    // Process a segment and try to reassemble
    bool processSegment(const RlcSegment& segment, std::vector<unsigned char>& reassembled_packet);
    
    // Cleanup expired reassembly contexts
    void cleanupExpired();
    
private:
    std::map<uint32_t, ReassemblyContext> reassembly_contexts;
    mutable std::mutex reassembly_mutex;
    uint32_t reassembly_timeout_ms;
};

// RLC Receiver Module class
class RlcReceiverModule {
public:
    RlcReceiverModule(const RlcReceiverConfig& cfg, const std::string& log_dir = "emulator_logs");
    ~RlcReceiverModule();
    
    // Process received segments
    bool processSegment(const unsigned char* segment_data, size_t len);
    
    // Enqueue a reassembled packet
    bool enqueueReassembledPacket(const unsigned char* packet, size_t len);
    
    // Dequeue a packet (up to maxSize bytes)
    std::shared_ptr<RlcPacket> dequeuePacket(size_t maxSize);
    
    // Check if there are packets available for dequeuing
    bool hasPackets() const;
    
    // Get current buffer occupancy
    size_t getBufferOccupancy() const;
    
    // Get buffer capacity
    size_t getBufferCapacity() const;
    
    // Set callback for delivering reassembled packets to higher layer
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);

    // Set multi-queue aware callback for delivering packets with queue routing information
    void setMultiQueueDeliveryCallback(std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> callback);
    
    // Set callback for sending status PDUs
    void setStatusPduCallback(std::function<void(const RlcStatusPdu&)> callback) {
        statusPduCallback = callback;
        LOG_INFO("[RLC Receiver] Status PDU callback set");
    }
    
    // Get statistics
    void getStats(uint32_t& reassembled, uint32_t& dequeued, uint32_t& dropped, uint32_t& segments_processed) const;
    
    // Helper to check if a packet is likely an RLC segment
    bool isValidSegmentHeader(const unsigned char* data);
    
    // Start the status PDU checking thread
    void startStatusPduThread();
    
    // Stop the status PDU checking thread
    void stopStatusPduThread();

private:
    // Buffer for reassembled packets
    moodycamel::ReaderWriterQueue<std::shared_ptr<RlcPacket>> packetBuffer;
    uint32_t maxBufferSize;
    
    // Statistics
    std::atomic<uint32_t> packetsReassembled;
    std::atomic<uint32_t> packetsDequeued;
    std::atomic<uint32_t> packetsDropped;
    std::atomic<uint32_t> segmentsProcessed;
    std::atomic<uint32_t> acksSent;
    
    // Flow control
    std::atomic<uint16_t> current_window_size;
    
    // Reassembly manager
    std::unique_ptr<RlcReassemblyManager> reassemblyManager;
    
    // Missing segments tracking
    std::mutex mutex;
    std::map<uint32_t, std::vector<uint16_t>> missing_segments;
    
    // Set of packet IDs that need Status PDUs
    std::set<uint32_t> pending_status_pdus;
    std::mutex status_pdu_mutex;
    std::condition_variable status_pdu_cv;
    
    // Callbacks
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> multiQueueDeliveryCallback;
    std::function<void(const RlcStatusPdu&)> statusPduCallback;
    
    // Status PDU thread control
    std::thread status_pdu_thread;
    std::atomic<bool> status_pdu_thread_running;
    uint32_t status_pdu_interval_ms;
    
    // Helper methods
    bool encapsulateAndDeliver(const unsigned char* packet, size_t len);
    bool encapsulateAndDeliver(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority);
    RlcStatusPdu createStatusPdu(uint32_t packet_id, bool is_complete);
    
    // Status PDU thread function
    void statusPduThreadFunc();
    
    // Process a segment internally and add to pending Status PDUs if needed
    bool processSegmentInternal(const RlcSegment& segment);
    
    // Track the highest sequence number seen for detecting out-of-order packets
    uint32_t highest_sn_seen;
    
    // Last time we sent a status report
    std::chrono::steady_clock::time_point last_status_report_time;

    std::atomic<bool> pending_poll;
    uint32_t t_statusProhibit_ms;
    
    // Logger for status PDU transmissions
    std::unique_ptr<AsyncLogger> status_pdu_logger;
};