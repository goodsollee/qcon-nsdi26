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
#include <functional>

#include "readerwriterqueue.h"
#include "log.hpp"
#include "rlc_segmentation.hpp"
#include "rlc_retransmission.hpp" 

// Maximum segment size for the RLC layer
#define RLC_MAX_SEGMENT_SIZE 1500 // Default max segment size
#define RLC_ACK_TYPE 0x01 // Type identifier for ACK packets

// RLC Packet Structure
struct RlcPacket {
    std::vector<unsigned char> data;
    size_t size;
    std::chrono::steady_clock::time_point timestamp;
    uint32_t packet_id; // Unique ID for tracking

    bool poll;
    uint8_t queue_id = 0;  // Queue ID for receiver routing
    uint8_t priority = 0;  // Priority for QoS handling

    RlcPacket(const unsigned char* buffer, size_t len);
};

// RLC Segmentation Manager - handles segmentation of large packets
class RlcSegmentationManager {
public:
    RlcSegmentationManager();
    ~RlcSegmentationManager();

    // Segment a packet into multiple RLC segments based on available bytes
    std::vector<std::shared_ptr<RlcSegment>> segmentPacket(const unsigned char* packet, size_t length, size_t max_segment_size, uint32_t packet_id);

private:
    std::atomic<uint32_t> next_packet_id;
};

// RLC Sender Module - Responsible for buffering and segmenting packets
class RlcSenderModule {
public:
    RlcSenderModule(uint32_t bufferSize = 100, 
                    uint32_t retransmission_timeout_ms = 1000,
                    uint16_t max_retries = 3, 
                    uint32_t retransmit_timer = 80,
                    const std::string& log_dir = "emulator_logs");
    ~RlcSenderModule();

    // Enqueue a packet
    bool enqueuePacket(const unsigned char* packet, size_t len);

    // Enqueue a packet with queue metadata
    bool enqueuePacket(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority);

    // Dequeue a packet that fits within the available bytes
    std::shared_ptr<RlcPacket> dequeuePacket(size_t availableBytes);

    // Process an incoming ACK
    bool processAck(const unsigned char* ack_data, size_t len);

    void checkPollTimer();

    // Handle retransmissions
    void checkRetransmissions();

    // Set maximum segment size
    void setMaxSegmentSize(size_t size);

    // Set callback for retransmitting packets
    void setRetransmissionCallback(std::function<bool(uint32_t)> callback);

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

    size_t getBufferSizeBytes() const;
    
    size_t getSegmentBufferSizeBytes() const;

    // Get statistics
    void getStats(uint32_t& buffered, uint32_t& dequeued, uint32_t& dropped,
                  uint32_t& segments_created, uint32_t& segments_dequeued) const;

    
    RlcRetransmissionManager* getRetransmissionManager() const { 
        return retransmissionManager.get(); 
    }
    
    // For RIC integration - access to timer mutex
    std::mutex& getTimerMutex() { 
        return timer_mutex; 
    }
    
    // For RIC integration - setter for poll bit
    void setPollBitPending(bool pending) { 
        poll_bit_pending = pending; 
    }

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

    // Retransmission manager
    std::unique_ptr<RlcRetransmissionManager> retransmissionManager;

    // Callback for retransmitting packets
    std::function<bool(uint32_t)> retransmissionCallback;

    // Mutex for protecting data
    std::mutex mutex;

    // t_pollRetransmit timer
    std::chrono::steady_clock::time_point t_pollRetransmit_expiry;
    uint32_t t_pollRetransmit_ms; // Timer value in milliseconds
    bool poll_bit_pending; // Flag to indicate if next packet needs poll bit
    std::mutex timer_mutex; // Protect timer-related variables

    std::atomic<size_t> totalBufferedBytes{0};
    std::atomic<size_t> totalSegmentBufferedBytes{0};
};

#endif // RLC_SENDER_HPP