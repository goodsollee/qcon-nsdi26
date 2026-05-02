#include "pdcp_common.hpp"
#include <chrono>   // for steady_clock
#include <cstring>  // for memset, memcpy

namespace pdcp {

/**
 * @brief Simple IPv4/TCP/UDP check
 */
bool shouldProcessPacket(const unsigned char* packet, size_t len) {
    // Check min size (IPv4 header = 20 bytes)
    if (len < 20) {
        return false;
    }
    // IPv4?
    if ((packet[0] >> 4) != 4) {
        return false;
    }
    // Protocol: TCP=6, UDP=17
    uint8_t protocol = packet[9];
    if (protocol != 6 && protocol != 17) {
        return false;
    }
    // For demonstration, only process bigger packets
    if ((protocol == 6 && len <= 40) || (protocol == 17 && len <= 28)) {
        return false;
    }
    return true;
}

/**
 * @brief Calculate IP checksum
 */
uint16_t calculateIpChecksum(void* vdata, size_t length) {
    uint16_t* data = static_cast<uint16_t*>(vdata);
    uint32_t sum = 0;

    while (length > 1) {
        sum += *data++;
        length -= 2;
    }
    if (length == 1) {
        sum += *reinterpret_cast<uint8_t*>(data);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

/**
 * @brief Analyze IPv4/TCP to extract seq
 */
bool analyzeTcp(const unsigned char* packet, size_t len, uint32_t& tcpSeq) {
    if (len < 40) {
        return false;
    }
    // Check IPv4
    if ((packet[0] >> 4) != 4) {
        return false;
    }
    // TCP?
    if (packet[9] != 6) {
        return false;
    }
    // IP header length
    uint8_t ihl = (packet[0] & 0x0F);
    size_t ip_header_len = ihl * 4;
    if (len < ip_header_len + 20) {
        return false;
    }

    const unsigned char* tcp_header = packet + ip_header_len;
    // Extract bytes 4..7
    tcpSeq = (static_cast<uint32_t>(tcp_header[4]) << 24) |
             (static_cast<uint32_t>(tcp_header[5]) << 16) |
             (static_cast<uint32_t>(tcp_header[6]) << 8)  |
             static_cast<uint32_t>(tcp_header[7]);
    return true;
}

/**
 * @brief Dump packet in hex
 */
std::string dumpPacketHex(const unsigned char* packet, size_t len, size_t max_bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    size_t bytes_to_dump = (len > max_bytes) ? max_bytes : len;
    for (size_t i = 0; i < bytes_to_dump; i++) {
        if (i > 0 && (i % 16) == 0) {
            ss << "\n";
        } else if (i > 0) {
            ss << " ";
        }
        ss << std::setw(2) << static_cast<int>(packet[i]);
    }
    if (len > max_bytes) {
        ss << "\n... (" << (len - max_bytes) << " more bytes)";
    }
    return ss.str();
}

/**
 * @brief Create PDCP header
 */
void createPdcpHeader(pdcp_hdr* header,
                      uint32_t epoch, uint32_t seq, uint64_t timestamp) {
    std::memset(header, 0, sizeof(pdcp_hdr));
    header->epoch = htonl(epoch);
    header->flags = 0;
    header->pad   = 0;
    header->reserved = 0;
    header->sequence_number = htonl(seq);
    uint64_t be_timestamp = htobe64(timestamp);
    std::memcpy(&header->send_timestamp, &be_timestamp, sizeof(uint64_t));
}

/**
 * @brief Parse PDCP header
 */
bool parsePdcpHeader(const pdcp_hdr* header,
                     uint32_t& epoch, uint32_t& seq, uint64_t& timestamp) {
    if (!header) {
        return false;
    }
    epoch = ntohl(header->epoch);
    seq   = ntohl(header->sequence_number);

    uint64_t be_timestamp;
    std::memcpy(&be_timestamp, &header->send_timestamp, sizeof(uint64_t));
    timestamp = be64toh(be_timestamp);
    return true;
}

/**
 * @brief Current time in ms
 */
uint64_t getCurrentTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Create PDCP packet
 */
size_t createPdcpPacket(unsigned char* buffer,
                        const unsigned char* ip_packet, size_t ip_len,
                        uint32_t epoch, uint32_t seq, uint8_t flags) {
    if (ip_len + sizeof(ipv4_hdr) + sizeof(pdcp_hdr) > PDCP_MAX_PACKET_SIZE) {
        return 0; // too large
    }
    size_t outerIPLen = sizeof(ipv4_hdr);
    size_t pdcpLen = sizeof(pdcp_hdr);
    size_t totalLen = outerIPLen + pdcpLen + ip_len;

    // Move the original IP
    std::memmove(buffer + outerIPLen + pdcpLen, ip_packet, ip_len);

    // PDCP header
    pdcp_hdr* pHdr = reinterpret_cast<pdcp_hdr*>(buffer + outerIPLen);
    uint64_t now_ms = getCurrentTimeMs();
    createPdcpHeader(pHdr, epoch, seq, now_ms);
    pHdr->flags = flags;

    // Outer IP
    auto* out_iph = reinterpret_cast<ipv4_hdr*>(buffer);
    std::memset(out_iph, 0, outerIPLen);
    out_iph->version  = 4;
    out_iph->ihl      = 5; // 20 bytes
    out_iph->ttl      = 64;
    out_iph->protocol = 4; // IP-in-IP
    out_iph->saddr    = htonl(0x0A640001); // 10.100.0.1
    out_iph->daddr    = htonl(0x09090909); // 9.9.9.9
    out_iph->tot_len  = htons(static_cast<uint16_t>(totalLen));
    out_iph->id       = htons(static_cast<uint16_t>(seq & 0xFFFF));

    // IP checksum
    out_iph->check = 0;
    out_iph->check = calculateIpChecksum(out_iph, outerIPLen);
    return totalLen;
}

/**
 * @brief Extract IP packet from PDCP
 */
size_t extractIpPacket(unsigned char* dest_buffer,
                       const unsigned char* pdcp_packet, size_t pdcp_len,
                       uint32_t& epoch, uint32_t& seq, uint64_t& timestamp) {
    // Need at least outer IP + PDCP
    if (pdcp_len < sizeof(ipv4_hdr) + sizeof(pdcp_hdr)) {
        return 0;
    }
    const ipv4_hdr* out_iph = reinterpret_cast<const ipv4_hdr*>(pdcp_packet);
    // Check IP-in-IP
    if (out_iph->protocol != 4) {
        return 0;
    }
    size_t outer_ip_len = out_iph->ihl * 4;
    if (pdcp_len < outer_ip_len + sizeof(pdcp_hdr)) {
        return 0;
    }
    // PDCP header
    const pdcp_hdr* pHdr =
        reinterpret_cast<const pdcp_hdr*>(pdcp_packet + outer_ip_len);
    if (!parsePdcpHeader(pHdr, epoch, seq, timestamp)) {
        return 0;
    }

    // Copy inner IP
    size_t inner_ip_len = pdcp_len - outer_ip_len - sizeof(pdcp_hdr);
    std::memcpy(dest_buffer,
                pdcp_packet + outer_ip_len + sizeof(pdcp_hdr),
                inner_ip_len);
    return inner_ip_len;
}

} // namespace pdcp
