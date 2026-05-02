#include "pdcp_common.hpp"
#include "pdcp_sender.hpp"
#include "pdcp_link.hpp"
#include "pdcp_receiver.hpp"
#include "log.h" 
#include <arpa/inet.h>
#include <cstring>
#include <iostream>

namespace pdcp {

bool shouldProcessPacket(const unsigned char* packet, size_t len) {
    // Basic validation
    if (len < 20) {
        return false;
    }
    
    const ipv4_hdr* iph = reinterpret_cast<const ipv4_hdr*>(packet);
    
    // Only process IPv4 packets
    if (iph->version != 4) {
        return false;
    }
    
    // For simplicity, we'll process all IPv4 packets
    // You might want to filter by source/destination IPs or protocols here
    return true;
}

uint16_t calculateIpChecksum(void* vdata, size_t length) {
    // Cast the data to uint16_t pointer
    uint16_t* data = static_cast<uint16_t*>(vdata);
    
    // Initialize sum to zero
    uint32_t sum = 0;
    
    // Main loop to sum up 16-bit words
    while (length > 1) {
        sum += *data++;
        length -= 2;
    }
    
    // Add left-over byte, if any
    if (length > 0) {
        sum += *(static_cast<uint8_t*>(vdata) + (length - 1));
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    
    // Take one's complement
    return static_cast<uint16_t>(~sum);
}

void dumpIpPacket(const unsigned char* buf, size_t len, const std::string& direction) {
    if (len < 20) {
        LOG_WARN("[" << direction << "] Packet too short: " << len << " bytes");
        return;
    }
    
    const ipv4_hdr* iph = reinterpret_cast<const ipv4_hdr*>(buf);
    
    if (iph->version != 4) {
        LOG_WARN("[" << direction << "] Not IPv4 packet (version=" << static_cast<int>(iph->version) << ")");
        return;
    }
    
    int ihl_bytes = iph->ihl * 4;
    if (ihl_bytes < 20 || ihl_bytes > static_cast<int>(len)) {
        LOG_WARN("[" << direction << "] Bad IHL=" << ihl_bytes << ", pkt_len=" << len);
        return;
    }

    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &iph->saddr, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &iph->daddr, dst_ip, sizeof(dst_ip));

    std::string proto_name;
    switch (iph->protocol) {
        case 1:  proto_name = "ICMP"; break;
        case 6:  proto_name = "TCP";  break;
        case 17: proto_name = "UDP";  break;
        default: proto_name = "OTHER"; break;
    }

    LOG_INFO("[" << direction << "] IPv4 src=" << src_ip
             << " -> dst=" << dst_ip
             << ", proto=" << static_cast<unsigned>(iph->protocol) 
             << "(" << proto_name << ")"
             << ", ttl=" << static_cast<unsigned>(iph->ttl)
             << ", tot_len=" << ntohs(iph->tot_len));
}

bool analyzeTcp(const unsigned char* packet, size_t len, uint32_t& seqOut)
{
    // 1) Must have at least an IPv4 header
    if (len < sizeof(ipv4_hdr)) {
        LOG_DEBUG("[analyzeTcp] Not enough bytes for IPv4 header. len=" << len);
        return false;
    }

    // 2) Parse the IPv4 header
    const ipv4_hdr* iph = reinterpret_cast<const ipv4_hdr*>(packet);
    if (iph->version != 4) {
        LOG_DEBUG("[analyzeTcp] Not an IPv4 packet. version=" << (int)iph->version);
        return false;
    }

    size_t ipHeaderLen = iph->ihl * 4;
    if (ipHeaderLen < 20 || ipHeaderLen > len) {
        LOG_DEBUG("[analyzeTcp] Invalid IPv4 header length. ipHeaderLen=" 
                  << ipHeaderLen << ", totalLen=" << len);
        return false;
    }

    // 3) Must be protocol=6 (TCP)
    if (iph->protocol != 6) {
        LOG_DEBUG("[analyzeTcp] Not a TCP packet. protocol=" << (int)iph->protocol);
        return false;
    }

    // 4) Check we have at least IP + minimal TCP (20 bytes)
    if (ipHeaderLen + 20 > len) {
        LOG_DEBUG("[analyzeTcp] Not enough data for minimal TCP. "
                  << "ipHeaderLen=" << ipHeaderLen 
                  << ", required=" << (ipHeaderLen + 20) 
                  << ", totalLen=" << len);
        return false;
    }

    // 5) Parse the TCP header
    const tcp_hdr* tcph = reinterpret_cast<const tcp_hdr*>(packet + ipHeaderLen);

    // TCP data offset is bits [15..12] in the 16-bit 'flags' field
    uint16_t flagsHost = ntohs(tcph->flags);
    uint8_t tcpDataOffset = (flagsHost >> 12) & 0x0F;
    size_t tcpHeaderLen = tcpDataOffset * 4;  // e.g., dataOffset=5 => 20 bytes

    // 6) Check that we have the full TCP header in this packet
    if (ipHeaderLen + tcpHeaderLen > len) {
        LOG_DEBUG("[analyzeTcp] Not enough data for the declared TCP header. "
                  << "ipHeaderLen=" << ipHeaderLen
                  << ", tcpHeaderLen=" << tcpHeaderLen
                  << ", totalLen=" << len);
        return false;
    }

    // 7) Parse the TCP sequence number
    seqOut = ntohl(tcph->seq);

    // 8) Final success logging
    LOG_DEBUG("[analyzeTcp] Parsed TCP packet: "
              << "ipHeaderLen=" << ipHeaderLen
              << ", tcpHeaderLen=" << tcpHeaderLen
              << ", seq=" << seqOut
              << ", totalLen=" << len);

    return true; // Successfully parsed
}


bool analyzeRtp(const unsigned char* packet, size_t len,
                uint32_t& timestampOut, bool& markerOut)
{
    // 1) Check basic lengths
    if (len < (sizeof(ipv4_hdr) + sizeof(udp_hdr) + sizeof(rtp_hdr))) {
        return false;
    }

    // 2) Parse IPv4 header
    const ipv4_hdr* iph = reinterpret_cast<const ipv4_hdr*>(packet);
    if (iph->version != 4) {
        return false;
    }

    size_t ipHeaderLen = iph->ihl * 4;
    if (ipHeaderLen < 20 || ipHeaderLen > len) {
        return false;
    }

    // 3) Check if it is UDP (protocol=17)
    if (iph->protocol != 17) { // 17 == UDP
        return false;
    }

    // 4) Parse UDP header
    if (ipHeaderLen + sizeof(udp_hdr) > len) {
        return false;
    }
    const udp_hdr* udph = reinterpret_cast<const udp_hdr*>(packet + ipHeaderLen);

    // 5) Check the UDP length
    uint16_t udpTotalLen = ntohs(udph->len);
    if (udpTotalLen < (sizeof(udp_hdr) + sizeof(rtp_hdr))) {
        // Not enough room for an RTP packet in this UDP payload
        return false;
    }

    // 6) The RTP header starts right after the UDP header
    const size_t rtpOffset = ipHeaderLen + sizeof(udp_hdr);
    if (rtpOffset + sizeof(rtp_hdr) > len) {
        return false;
    }

    const rtp_hdr* rtph = reinterpret_cast<const rtp_hdr*>(packet + rtpOffset);

    // 7) Extract RTP fields (converting to host order where necessary)
    timestampOut = ntohl(rtph->timestamp);

    // Marker bit is the highest bit of the second byte in `flags`
    // e.g. if 'flags' is 0x80 0xE0, the top bit of 0xE0 is marker.
    uint16_t flagsHost = ntohs(rtph->flags);
    // The marker is bit 8 (top bit of the low byte).
    markerOut = (flagsHost & 0x0080) != 0; 

    return true;
}

long timespecDiffMs(const struct timespec* start, const struct timespec* end) {
    return (end->tv_sec - start->tv_sec) * 1000 + 
           (end->tv_nsec - start->tv_nsec) / 1000000;
}

} // namespace pdcp
