#include "rlc_segmentation.hpp"
#include <cstring>
#include <arpa/inet.h> // htonl/ntohl etc.
#include "log.hpp"

// ---------------------
// RlcSegmentHeader
// ---------------------
std::vector<unsigned char> RlcSegmentHeader::serialize() const {
    std::vector<unsigned char> result(RLC_SEGMENT_HEADER_SIZE); // 22 bytes

    // convert to network byte order
    uint32_t net_packet_id       = htonl(original_packet_id);
    uint16_t net_segment_number  = htons(segment_number);
    uint16_t net_total_segments  = htons(total_segments);
    uint32_t net_original_size   = htonl(original_size);
    uint16_t net_segment_size    = htons(segment_size);
    uint32_t net_offset_in_orig  = htonl(offset_in_original);

    // copy fields
    memcpy(result.data(),       &net_packet_id,      4);
    memcpy(result.data() + 4,   &net_segment_number, 2);
    memcpy(result.data() + 6,   &net_total_segments, 2);
    memcpy(result.data() + 8,   &net_original_size,  4);
    memcpy(result.data() + 12,  &net_segment_size,   2);

    // is_last_segment -> 1 byte
    result[14] = is_last_segment ? 1 : 0;

    // poll flag
    result[15] = poll ? 1 : 0;

    // offset_in_original -> 4 bytes
    memcpy(result.data() + 16, &net_offset_in_orig, 4);

    // queue_id and priority -> 1 byte each
    result[20] = queue_id;
    result[21] = priority;

    return result;
}

RlcSegmentHeader RlcSegmentHeader::deserialize(const unsigned char* data) {
    RlcSegmentHeader header;

    uint32_t net_packet_id;
    uint16_t net_segment_number;
    uint16_t net_total_segments;
    uint32_t net_original_size;
    uint16_t net_segment_size;
    uint32_t net_offset_in_orig;

    memcpy(&net_packet_id,      data,     4);
    memcpy(&net_segment_number, data + 4, 2);
    memcpy(&net_total_segments, data + 6, 2);
    memcpy(&net_original_size,  data + 8, 4);
    memcpy(&net_segment_size,   data + 12,2);

    header.original_packet_id = ntohl(net_packet_id);
    header.segment_number     = ntohs(net_segment_number);
    header.total_segments     = ntohs(net_total_segments);
    header.original_size      = ntohl(net_original_size);
    header.segment_size       = ntohs(net_segment_size);
    header.is_last_segment    = (data[14] == 1);
    header.poll               = (data[15] == 1);

    memcpy(&net_offset_in_orig, data + 16, 4);
    header.offset_in_original  = ntohl(net_offset_in_orig);

    // Extract queue_id and priority
    header.queue_id  = data[20];
    header.priority  = data[21];

    return header;
}

// ---------------------
// RlcSegment
// ---------------------
RlcSegment::RlcSegment() 
    : timestamp(std::chrono::steady_clock::now()) 
{
    // nothing
}

RlcSegment::RlcSegment(uint32_t packet_id, uint16_t segment_number, uint16_t total_segments,
                       uint32_t original_size, bool is_last,
                       const unsigned char* segment_data, size_t data_len)
    : timestamp(std::chrono::steady_clock::now())
{
    header.original_packet_id = packet_id;
    header.segment_number     = segment_number;
    header.total_segments     = total_segments;
    header.original_size      = original_size;
    header.segment_size       = data_len;
    header.is_last_segment    = is_last;
    header.poll               = false; // Default to false, can be set separately
    // offset_in_original 은 송신 로직(RlcSegmentationManager)에서 설정할 것

    data.assign(segment_data, segment_data + data_len);
}

std::vector<unsigned char> RlcSegment::serialize() const {
    std::vector<unsigned char> hdr = header.serialize();
    std::vector<unsigned char> result(hdr.size() + data.size());

    // copy header
    memcpy(result.data(), hdr.data(), hdr.size());

    // copy data
    if (!data.empty()) {
        memcpy(result.data() + hdr.size(), data.data(), data.size());
    }

    return result;
}

RlcSegment RlcSegment::deserialize(const unsigned char* buffer, size_t length) {
    RlcSegment segment;
    if (length < RLC_SEGMENT_HEADER_SIZE) {
        LOG_ERROR("[RLC Segment] Buffer too small for deserialization");
        return segment;
    }

    segment.header = RlcSegmentHeader::deserialize(buffer);

    // copy data
    size_t payload_len = length - RLC_SEGMENT_HEADER_SIZE;
    if (payload_len > 0 && segment.header.segment_size > 0) {
        // 혹시나 segment_size != payload_len일 경우도 체크 가능
        size_t real_copy = std::min<size_t>(payload_len, segment.header.segment_size);
        segment.data.assign(buffer + RLC_SEGMENT_HEADER_SIZE,
                            buffer + RLC_SEGMENT_HEADER_SIZE + real_copy);
    }

    return segment;
}