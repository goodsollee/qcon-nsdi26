#ifndef PDCP_COMMON_HPP
#define PDCP_COMMON_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <time.h>
#include <iostream>
#include <memory>
#include <arpa/inet.h>

#include "log.h"

#define PDCP_SEQ_LIMIT 4294967295U

// Maximum packet size
#define PDCP_MAX_PACKET_SIZE 65535

// PDCP header structure (to be inserted before IP header)
struct pdcp_hdr {
    uint32_t  epoch;            // new: epoch or “wrap counter”
    uint8_t  flags;
    uint8_t  pad;             // 1 => Now total 8
    uint16_t reserved;         // for alignment or future
    uint32_t sequence_number;  // 32-bit
    uint64_t  send_timestamp;   // new: in ms (host -> network) 
} __attribute__((packed));

// PDCP roles
enum class PdcpRole {
    NONE = 0,
    SENDER,
    LINK,
    RECEIVER
};

// IPv4 header structure (simplified for reference)
struct ipv4_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    unsigned int ihl:4;
    unsigned int version:4;
#elif __BYTE_ORDER == __BIG_ENDIAN
    unsigned int version:4;
    unsigned int ihl:4;
#else
#error "Byte order not supported"
#endif
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

/**
 * Minimal TCP header structure for analysis.
 * Note: Marking it `packed` ensures no compiler-added padding.
 */
#pragma pack(push, 1)
struct tcp_hdr {
    uint16_t source;      // Source port
    uint16_t dest;        // Destination port
    uint32_t seq;         // Sequence number
    uint32_t ack_seq;     // Acknowledgment number
    uint16_t flags;       // Includes data offset + flags
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct udp_hdr {
    uint16_t source; // Source port
    uint16_t dest;   // Destination port
    uint16_t len;    // UDP length (header + data)
    uint16_t check;  // UDP checksum
};

struct rtp_hdr {
    uint16_t flags;         // V, P, X, CC, M, PT are stuffed in 16 bits
    uint16_t sequence_num;   // RTP sequence number
    uint32_t timestamp;      // RTP timestamp
    uint32_t ssrc;           // Synchronization source
};
#pragma pack(pop)


// Base class for all PDCP contexts
class PdcpContext {
public:
    PdcpContext(PdcpRole role) : role_(role) {}
    virtual ~PdcpContext() = default;
    
    // Process a packet based on role
    virtual size_t processPacket(unsigned char* packet, size_t len) = 0;
    
    PdcpRole getRole() const { return role_; }
    
protected:
    PdcpRole role_;
};

// Common functions
namespace pdcp {
    // Check if this is an IPv4 packet that should be processed by PDCP
    bool shouldProcessPacket(const unsigned char* packet, size_t len);
    
    // Calculate IPv4 header checksum
    uint16_t calculateIpChecksum(void* vdata, size_t length);
    
    // Helper for debugging to dump IP packet information
    void dumpIpPacket(const unsigned char* buf, size_t len, const std::string& direction);

    bool analyzeTcp(const unsigned char* packet, size_t len, uint32_t& seqOut);

    bool analyzeRtp(const unsigned char* packet, size_t len,
                uint32_t& timestampOut, bool& markerOut);
    
    // Calculate time difference in milliseconds
    long timespecDiffMs(const struct timespec* start, const struct timespec* end);
}

#endif /* PDCP_COMMON_HPP */