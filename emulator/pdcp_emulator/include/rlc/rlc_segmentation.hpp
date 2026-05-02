#ifndef RLC_SEGMENTATION_HPP
#define RLC_SEGMENTATION_HPP

#include <vector>
#include <cstdint>
#include <memory>
#include <chrono>
#include <arpa/inet.h>

// RLC Segment Header
struct RlcSegmentHeader {
    uint32_t original_packet_id;  // ID of the original packet
    uint16_t segment_number;      // Sequence number of this segment
    uint16_t total_segments;      // Total number of segments for this packet
    uint32_t original_size;       // Original packet size before segmentation
    uint16_t segment_size;        // Size of this segment's data
    bool is_last_segment;         // Flag to indicate the last segment
    
    // Serialize header to bytes
    std::vector<unsigned char> serialize() const;
    
    // Deserialize header from bytes
    static RlcSegmentHeader deserialize(const unsigned char* data);
};

// RLC Segment - represents a segment of an RLC packet
struct RlcSegment {
    RlcSegmentHeader header;
    std::vector<unsigned char> data;
    std::chrono::steady_clock::time_point timestamp;
    
    RlcSegment();
    
    RlcSegment(uint32_t packet_id, uint16_t segment_number, uint16_t total_segments, 
               uint32_t original_size, bool is_last, const unsigned char* segment_data, size_t data_len);
    
    // Serialize the entire segment (header + data)
    std::vector<unsigned char> serialize() const;
    
    // Deserialize from raw bytes
    static RlcSegment deserialize(const unsigned char* buffer, size_t length);
};

#endif // RLC_SEGMENTATION_HPP