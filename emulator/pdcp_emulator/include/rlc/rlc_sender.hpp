#ifndef RLC_SENDER_HPP
#define RLC_SENDER_HPP

#include <queue>
#include <vector>
#include <mutex>
#include <cstdint>
#include <memory>
#include <chrono>
#include <atomic>
#include <deque>

#include "readerwriterqueue.h"
#include "log.h"
#include "rlc_segmentation.hpp"

// Maximum segment size for the RLC layer
#define RLC_MAX_SEGMENT_SIZE 1500 // Default max segment size

// RLC Packet Structure
struct RlcPacket {
    std::vector<unsigned char> data;
    size_t size;
    std::chrono::steady_clock::time_point timestamp;
    uint32_t packet_id; // Unique ID for tracking
    
    RlcPacket(const unsigned char* buffer, size_t len);
};

// RLC Segmentation Manager - handles segmentation of large packets
class RlcSegmentationManager {
public:
    RlcSegmentationManager();
    ~RlcSegmentationManager();
    
    // Segment a packet into multiple RLC segments based on available bytes
    std::vector<std::shared_ptr<RlcSegment>> segmentPacket(const unsigned char* packet, size_t length, size_t max_segment_size);

private:
    std::atomic<uint32_t> next_packet_id;
};

// RLC Sender Module - Responsible for buffering and segmenting packets
class RlcSenderModule {
public:
    RlcSenderModule(uint32_t bufferSize = 100);
    ~RlcSenderModule();
    
    // Enqueue a packet
    bool enqueuePacket(const unsigned char* packet, size_t len);
    
    // Dequeue a packet that fits within the available bytes
    std::shared_ptr<RlcPacket> dequeuePacket(size_t availableBytes);
    
    // Set maximum segment size
    void setMaxSegmentSize(size_t size);
    
    // Check if buffer has packets
    bool hasPackets() const;
    
    // Get current buffer occupancy
    size_t getBufferOccupancy() const;
    
    // Get segment buffer occupancy
    size_t getSegmentBufferOccupancy() const;
    
    // Get front packet size without removing it
    size_t getFrontPacketSize() const;
    
    // Get buffer capacity
    size_t getBufferCapacity() const;
    
    // Get statistics
    void getStats(uint32_t& buffered, uint32_t& dequeued, uint32_t& dropped,
                  uint32_t& segments_created, uint32_t& segments_dequeued) const;

private:
    moodycamel::ReaderWriterQueue<std::shared_ptr<RlcPacket>> packetBuffer;
    moodycamel::ReaderWriterQueue<std::shared_ptr<RlcSegment>> segmentBuffer;
    uint32_t maxBufferSize;
    std::atomic<uint32_t> packetsDropped;
    std::atomic<uint32_t> packetsBuffered;
    std::atomic<uint32_t> packetsDequeued;
    std::atomic<uint32_t> segmentsCreated;
    std::atomic<uint32_t> segmentsDequeued;
    std::atomic<uint32_t> next_packet_id;
    size_t maxSegmentSize;
    
    // Segmentation manager
    std::unique_ptr<RlcSegmentationManager> segmentationManager;
};

#endif // RLC_SENDER_HPP