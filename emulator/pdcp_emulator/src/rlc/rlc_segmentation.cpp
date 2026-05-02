#include "rlc_segmentation.hpp"
#include "log.h"
#include <cstring>

// RlcSegmentHeader methods
std::vector<unsigned char> RlcSegmentHeader::serialize() const {
    std::vector<unsigned char> result(16); // Header size is 16 bytes
    
    // Convert to network byte order and copy to vector
    uint32_t net_packet_id = htonl(original_packet_id);
    uint16_t net_segment_number = htons(segment_number);
    uint16_t net_total_segments = htons(total_segments);
    uint32_t net_original_size = htonl(original_size);
    uint16_t net_segment_size = htons(segment_size);
    
    memcpy(result.data(), &net_packet_id, sizeof(uint32_t));
    memcpy(result.data() + 4, &net_segment_number, sizeof(uint16_t));
    memcpy(result.data() + 6, &net_total_segments, sizeof(uint16_t));
    memcpy(result.data() + 8, &net_original_size, sizeof(uint32_t));
    memcpy(result.data() + 12, &net_segment_size, sizeof(uint16_t));
    result[14] = is_last_segment ? 1 : 0;
    result[15] = 0; // Reserved for future use
    
    return result;
}

RlcSegmentHeader RlcSegmentHeader::deserialize(const unsigned char* data) {
    RlcSegmentHeader header;
    
    uint32_t net_packet_id;
    uint16_t net_segment_number;
    uint16_t net_total_segments;
    uint32_t net_original_size;
    uint16_t net_segment_size;
    
    memcpy(&net_packet_id, data, sizeof(uint32_t));
    memcpy(&net_segment_number, data + 4, sizeof(uint16_t));
    memcpy(&net_total_segments, data + 6, sizeof(uint16_t));
    memcpy(&net_original_size, data + 8, sizeof(uint32_t));
    memcpy(&net_segment_size, data + 12, sizeof(uint16_t));
    
    header.original_packet_id = ntohl(net_packet_id);
    header.segment_number = ntohs(net_segment_number);
    header.total_segments = ntohs(net_total_segments);
    header.original_size = ntohl(net_original_size);
    header.segment_size = ntohs(net_segment_size);
    header.is_last_segment = data[14] == 1;
    
    return header;
}

// RlcSegment methods
RlcSegment::RlcSegment() 
    : timestamp(std::chrono::steady_clock::now()) {}

RlcSegment::RlcSegment(uint32_t packet_id, uint16_t segment_number, uint16_t total_segments, 
           uint32_t original_size, bool is_last, const unsigned char* segment_data, size_t data_len) 
    : timestamp(std::chrono::steady_clock::now()) {
    
    header.original_packet_id = packet_id;
    header.segment_number = segment_number;
    header.total_segments = total_segments;
    header.original_size = original_size;
    header.segment_size = data_len;
    header.is_last_segment = is_last;
    
    data.assign(segment_data, segment_data + data_len);
}

std::vector<unsigned char> RlcSegment::serialize() const {
    std::vector<unsigned char> header_bytes = header.serialize();
    std::vector<unsigned char> result(header_bytes.size() + data.size());
    
    // Copy header
    memcpy(result.data(), header_bytes.data(), header_bytes.size());
    
    // Copy data
    if (!data.empty()) {
        memcpy(result.data() + header_bytes.size(), data.data(), data.size());
    }
    
    return result;
}

RlcSegment RlcSegment::deserialize(const unsigned char* buffer, size_t length) {
    if (length < 16) { // Header size
        LOG_ERROR("[RLC Segment] Buffer too small for deserialization");
        return RlcSegment();
    }
    
    RlcSegment segment;
    segment.header = RlcSegmentHeader::deserialize(buffer);
    
    // Copy data if present
    if (length > 16 && segment.header.segment_size > 0) {
        segment.data.assign(buffer + 16, buffer + length);
    }
    
    return segment;
}