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
#include <iomanip>
#include <sstream>
#include <endian.h>
#include <chrono>

// Logging includes
#include "log.hpp"
#include "async_logger.hpp"


#define MODULE_SENDER "PdcpSender"
#define MODULE_DU "PdcpDU"
#define MODULE_UE "PdcpUE"
#define MODULE_RECEIVER "PdcpReceiver"


// PDCP roles
enum class PdcpRole {
    NONE = 0,
    SENDER,
    LINK,
    RECEIVER
};

// Some constants
#define PDCP_MAX_PACKET_SIZE 65535
#define PDCP_SEQ_LIMIT 4294967295U

// PDCP header
struct pdcp_hdr {
    uint64_t  epoch;          // Wrap counter
    uint8_t   flags;
    uint8_t   pad;
    uint16_t  reserved;       
    uint64_t  sequence_number;
    uint64_t  send_timestamp; 
} __attribute__((packed));

// IPv4 header structure (simplified)
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
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

// Minimal TCP header
#pragma pack(push, 1)
struct tcp_hdr {
    uint16_t source;
    uint16_t dest;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t flags;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
};
#pragma pack(pop)

// Minimal UDP + RTP
#pragma pack(push, 1)
struct udp_hdr {
    uint16_t source;
    uint16_t dest;
    uint16_t len;
    uint16_t check;
};

struct rtp_hdr {
    uint16_t flags;
    uint16_t sequence_num;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

// Base PdcpContext class
class PdcpContext {
public:
    PdcpContext(PdcpRole role) : role_(role) {}
    virtual ~PdcpContext() = default;

    // Process a packet
    virtual size_t processPacket(unsigned char* packet, size_t len) = 0;

    PdcpRole getRole() const { return role_; }

protected:
    PdcpRole role_;
};

// --------------------------------------------------------------------------
// pdcp namespace: function prototypes (non-inline)
// --------------------------------------------------------------------------
namespace pdcp {
/**
 * @brief Check if this IP packet is IPv4 and either TCP or UDP 
 */
bool shouldProcessPacket(const unsigned char* packet, size_t len);

/**
 * @brief Calculate IPv4 checksum
 */
uint16_t calculateIpChecksum(void* vdata, size_t length);

/**
 * @brief Analyze an IPv4/TCP packet to extract the TCP sequence number
 */
bool analyzeTcp(const unsigned char* packet, size_t len, uint32_t& tcpSeq);

/**
 * @brief Dump a packet in hex for debugging
 */
std::string dumpPacketHex(const unsigned char* packet, size_t len, size_t max_bytes);

/**
 * @brief Create a PDCP header
 */
void createPdcpHeader(pdcp_hdr* header, uint32_t epoch, uint32_t seq, uint64_t timestamp);

/**
 * @brief Parse a PDCP header
 */
bool parsePdcpHeader(const pdcp_hdr* header, uint32_t& epoch, uint32_t& seq, uint64_t& timestamp);

/**
 * @brief Get current time in milliseconds
 */
uint64_t getCurrentTimeMs();

/**
 * @brief Create a PDCP packet (outer IP + PDCP + inner IP)
 */
size_t createPdcpPacket(unsigned char* buffer,
                        const unsigned char* ip_packet, size_t ip_len,
                        uint32_t epoch, uint32_t seq, uint8_t flags=0);

/**
 * @brief Extract IP from PDCP (remove outer IP + PDCP)
 */
size_t extractIpPacket(unsigned char* dest_buffer,
                       const unsigned char* pdcp_packet, size_t pdcp_len,
                       uint32_t& epoch, uint32_t& seq, uint64_t& timestamp);

} // namespace pdcp

#endif // PDCP_COMMON_HPP
