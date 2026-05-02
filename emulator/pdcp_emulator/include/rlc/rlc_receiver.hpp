#ifndef RLC_RECEIVER_HPP
#define RLC_RECEIVER_HPP

#include <map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>

#include "readerwriterqueue.h"
#include "log.h"
#include "rlc_segmentation.hpp"

// RLC Packet Structure (same as in sender for consistency)
struct RlcReceiverPacket {
    std::vector<unsigned char> data;
    size_t size;
    std::chrono::steady_clock::time_point timestamp;
    uint32_t packet_id; // Unique ID for tracking
    
    RlcReceiverPacket(const unsigned char* buffer, size_t len);
};

// RLC Reassembly Manager - handles reassembly of segmented packets
class RlcReassemblyManager {
public:
    RlcReassemblyManager(uint32_t timeout_ms = 1000);
    ~RlcReassemblyManager();
    
    // Process an incoming segment, returns true if a complete packet is now available
    bool processSegment(const RlcSegment& segment, std::vector<unsigned char>& reassembled_packet);
    
    // Clean up expired reassembly contexts
    void cleanupExpired();

private:
    struct ReassemblyContext {
        uint32_t original_size;
        uint16_t total_segments;
        uint16_t segments_received;
        std::chrono::steady_clock::time_point last_update;
        std::vector<unsigned char> data;
        std::vector<bool> segment_received;
    };
    
    std::map<uint32_t, ReassemblyContext> reassembly_contexts;
    std::mutex reassembly_mutex;
    uint32_t reassembly_timeout_ms;
};

// RLC Receiver Module - Responsible for reassembling segmented packets
class RlcReceiverModule {
public:
    RlcReceiverModule(uint32_t bufferSize = 100, uint32_t reassembly_timeout_ms = 1000);
    ~RlcReceiverModule();
    
    bool isValidSegmentHeader(const unsigned char* data);

    bool encapsulateAndDeliver(const unsigned char* data, size_t len);

    // Process a received segment
    bool processSegment(const unsigned char* segment_data, size_t len);
    
    // Dequeue a reassembled packet
    std::shared_ptr<RlcReceiverPacket> dequeuePacket(size_t maxSize = UINT32_MAX);
    
    // Check if there are complete packets available
    bool hasPackets() const;
    
    // Get current buffer occupancy
    size_t getBufferOccupancy() const;
    
    // Get buffer capacity
    size_t getBufferCapacity() const;
    
    // Set callback for delivering reassembled packets
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Get statistics
    void getStats(uint32_t& reassembled, uint32_t& dequeued, uint32_t& dropped,
                  uint32_t& segments_processed) const;

private:
    moodycamel::ReaderWriterQueue<std::shared_ptr<RlcReceiverPacket>> packetBuffer;
    uint32_t maxBufferSize;
    std::atomic<uint32_t> packetsReassembled;
    std::atomic<uint32_t> packetsDequeued;
    std::atomic<uint32_t> packetsDropped;
    std::atomic<uint32_t> segmentsProcessed;
    
    std::unique_ptr<RlcReassemblyManager> reassemblyManager;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    std::mutex mutex;
    
    // Helper method to enqueue a reassembled packet
    bool enqueueReassembledPacket(const unsigned char* packet, size_t len);
};

#endif // RLC_RECEIVER_HPP