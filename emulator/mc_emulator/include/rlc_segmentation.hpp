#pragma once
#include <cstdint>
#include <vector>
#include <chrono>
#include <arpa/inet.h>


#define RLC_SEGMENT_HEADER_SIZE 22  // Increased for queue_id and priority fields
#define RLC_SEQ_NUM_MASK 0x0FFF  // Mask for sequence number
// ---------------------------------------------------
// RlcSegmentHeader
// ---------------------------------------------------
struct RlcSegmentHeader {
    uint32_t original_packet_id;
    uint16_t segment_number;
    uint16_t total_segments;
    uint32_t original_size;
    uint16_t segment_size;
    bool     is_last_segment;

    uint32_t offset_in_original;

    bool poll;
    uint8_t queue_id = 0;   // Queue ID for receiver routing
    uint8_t priority = 0;   // Priority for QoS handling

    std::vector<unsigned char> serialize() const;

    static RlcSegmentHeader deserialize(const unsigned char* data);
};

// ---------------------------------------------------
// RlcSegment
// ---------------------------------------------------
class RlcSegment {
public:
    RlcSegment();
    RlcSegment(uint32_t packet_id, uint16_t segment_number, uint16_t total_segments,
               uint32_t original_size, bool is_last,
               const unsigned char* segment_data, size_t data_len);

    std::vector<unsigned char> serialize() const;

    static RlcSegment deserialize(const unsigned char* buffer, size_t length);

    RlcSegmentHeader header;
    std::vector<unsigned char> data;
    std::chrono::steady_clock::time_point timestamp;
};

