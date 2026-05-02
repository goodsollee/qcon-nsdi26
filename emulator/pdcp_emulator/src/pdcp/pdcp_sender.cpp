#include "pdcp_sender.hpp"
#include <arpa/inet.h>
#include <iostream>
#include <cstring>
#include <chrono>

PdcpSender::PdcpSender(const PdcpConfig& cfg)
    : PdcpContext(PdcpRole::SENDER),
      nextSequence(0),
      currentEpoch (0),
      async_logger(cfg.common.logFoldername + "/pdcp_sender.csv")
{
    LOG_INFO("[PDCP Sender] Initialized");

    // Write CSV header
    async_logger.logLine("pdcp_seq,tcp_seq,timestamp,pkt_len");
}

PdcpSender::~PdcpSender() {
    LOG_INFO("[PDCP Sender] Cleaned up, final sequence: " << nextSequence.load());
}

size_t PdcpSender::processPacket(unsigned char* packet, size_t len)
{
    // 1) Check if we should process
    if (!pdcp::shouldProcessPacket(packet, len)) {
        return len; // pass through unmodified
    }

    // 2) Copy the original (inner) IP packet to parse it
    std::vector<unsigned char> innerIP(packet, packet + len);
    if (len < sizeof(ipv4_hdr)) {
        // Can't parse properly, just pass
        return len;
    }

    // --- NEW: Attempt to parse TCP sequence from the inner IP. ---
    uint32_t tcpSeq = 0;
    bool isTcp = pdcp::analyzeTcp(innerIP.data(), len, tcpSeq);

    // 3) Prepare [outer IP + PDCP + inner IP]
    size_t outerIPLen = sizeof(ipv4_hdr);
    size_t pdcpLen    = sizeof(pdcp_hdr);
    size_t totalLen   = outerIPLen + pdcpLen + len;
    if (totalLen > PDCP_MAX_PACKET_SIZE) {
        LOG_WARN("[PDCP Sender] Packet too large after PDCP encaps. Dropping.");
        return 0;
    }

    // Shift the original IP data to make room
    std::memmove(packet + outerIPLen + pdcpLen, innerIP.data(), len);

    // 4) Fill in PDCP header
    pdcp_hdr* pHdr = reinterpret_cast<pdcp_hdr*>(packet + outerIPLen);
    std::memset(pHdr, 0, sizeof(pdcp_hdr));

    // --- Retrieve and increment the PDCP sequence ---
    uint32_t seq = nextSequence.fetch_add(1, std::memory_order_relaxed);
    if (seq > PDCP_SEQ_LIMIT) {
        // Wrap
        nextSequence.store(0, std::memory_order_relaxed);
        currentEpoch.fetch_add(1, std::memory_order_relaxed);
        seq = 0;
    }
    uint32_t epoch = currentEpoch.load(std::memory_order_relaxed);

    pHdr->epoch           = htonl(epoch);
    pHdr->flags           = 0;
    pHdr->reserved        = 0;
    pHdr->sequence_number = htonl(seq);

    // 5) Insert "send time" in ms
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    uint64_t beTime = htobe64(static_cast<uint64_t>(now_ms));
    std::memcpy(&pHdr->send_timestamp, &beTime, sizeof(uint64_t));

    // --- NEW: Log CSV: (pdcp_seq, tcp_seq, timestamp) ---
    {
        std::stringstream ss;
        ss << seq << "," 
           << (isTcp ? std::to_string(tcpSeq) : "N/A") << ","
           << now_ms <<","
           << len;
        async_logger.logLine(ss.str());
    }

    LOG_DEBUG("[PDCP Sender] PDCP epoch=" << epoch
              << " seq=" << seq
              << " totalLen=" << totalLen);

    // 6) Fill the "outer IP header"
    auto* out_iph = reinterpret_cast<ipv4_hdr*>(packet);
    std::memset(out_iph, 0, outerIPLen);

    out_iph->version  = 4;
    out_iph->ihl      = 5;  // 5 * 4 = 20 bytes
    out_iph->ttl      = 64;
    out_iph->protocol = 4;  // IPPROTO_IPIP

    // Re-use the original addresses
    out_iph->saddr = htonl(0x0A640001); // 10.100.0.1
    out_iph->daddr = htonl(0x09090909); // 9.9.9.9

    // totalLen might exceed 16-bit if huge, but let's assume it doesn't
    out_iph->tot_len = htons(static_cast<uint16_t>(totalLen));

    // Simple ID
    out_iph->id = htons(static_cast<uint16_t>(seq & 0xFFFF));

    // 7) Compute outer IP checksum
    out_iph->check = 0;
    out_iph->check = pdcp::calculateIpChecksum(out_iph, outerIPLen);

    // 8) Return the new total length
    LOG_DEBUG("PDCP encapsulated packet length: " << totalLen);
    return totalLen;
}

uint32_t PdcpSender::getSequence() const {
    return nextSequence.load(std::memory_order_relaxed);
}