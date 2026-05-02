#include "qos_profiler.hpp"
#include "log.hpp"
#include <cstring>
#include <arpa/inet.h>

#define MODULE "QosProfiler"

QosProfiler::QosProfiler(ClassificationMode mode, uint8_t numQueues)
    : mode_(mode),
      numQueues_(numQueues),
      roundRobinCounter_(0)
{
    // Initialize statistics
    resetStats();

    LOG_MODULE_INFO(MODULE, "QoS Profiler initialized with "
                    << static_cast<int>(numQueues_) << " queues, mode="
                    << static_cast<int>(mode_));
}

QosProfiler::~QosProfiler()
{
    LOG_MODULE_INFO(MODULE, "QoS Profiler destroyed. Final statistics:");
    for (uint8_t i = 0; i < numQueues_; i++) {
        LOG_MODULE_INFO(MODULE, "  Queue " << static_cast<int>(i)
                        << ": " << queueStats_[i] << " packets");
    }
}

uint8_t QosProfiler::classifyPacket(const unsigned char* packet, size_t len)
{
    if (!packet || len == 0) {
        LOG_MODULE_WARN(MODULE, "Invalid packet data, using queue 0");
        return 0;
    }

    uint8_t queueId = 0;

    switch (mode_) {
        case ClassificationMode::ROUND_ROBIN:
            queueId = classifyRoundRobin();
            break;

        case ClassificationMode::SEQUENCE_MOD:
            queueId = classifyBySequence(packet, len);
            break;

        case ClassificationMode::PORT_BASED:
            queueId = classifyByPort(packet, len);
            break;

        case ClassificationMode::PACKET_SIZE:
            queueId = classifyBySize(len);
            break;

        case ClassificationMode::DSCP_BASED:
            // For now, fallback to sequence-based
            LOG_MODULE_DEBUG(MODULE, "DSCP classification not implemented, using sequence-based");
            queueId = classifyBySequence(packet, len);
            break;

        default:
            LOG_MODULE_WARN(MODULE, "Unknown classification mode, using queue 0");
            queueId = 0;
            break;
    }

    // Ensure queue ID is within valid range
    if (queueId >= numQueues_) {
        queueId = 0;
    }

    // Update statistics
    queueStats_[queueId]++;

    LOG_MODULE_DEBUG(MODULE, "Packet classified to queue " << static_cast<int>(queueId)
                     << " (mode=" << static_cast<int>(mode_)
                     << ", len=" << len << ")");

    return queueId;
}

uint8_t QosProfiler::classifyRoundRobin()
{
    uint8_t queueId = static_cast<uint8_t>(roundRobinCounter_ % numQueues_);
    roundRobinCounter_++;
    return queueId;
}

uint8_t QosProfiler::classifyBySequence(const unsigned char* packet, size_t len)
{
    if (len < sizeof(pdcp_hdr)) {
        LOG_MODULE_WARN(MODULE, "Packet too small for PDCP header, using queue 0");
        return 0;
    }

    const pdcp_hdr* header = reinterpret_cast<const pdcp_hdr*>(packet);
    uint32_t sequence = ntohl(header->sequence_number);

    uint8_t queueId = static_cast<uint8_t>(sequence % numQueues_);

    LOG_MODULE_DEBUG(MODULE, "Sequence-based classification: seq=" << sequence
                     << " -> queue=" << static_cast<int>(queueId));

    return queueId;
}

uint8_t QosProfiler::classifyByPort(const unsigned char* packet, size_t len)
{
    // Skip PDCP header to get to IP packet
    if (len < sizeof(pdcp_hdr) + sizeof(ipv4_hdr)) {
        LOG_MODULE_DEBUG(MODULE, "Packet too small for IP header analysis");
        return classifyBySequence(packet, len);  // Fallback
    }

    const unsigned char* ipPacket = packet + sizeof(pdcp_hdr);
    const ipv4_hdr* ipHeader = reinterpret_cast<const ipv4_hdr*>(ipPacket);

    // Check if it's TCP or UDP
    if (ipHeader->protocol == 6) {  // TCP
        if (len < sizeof(pdcp_hdr) + sizeof(ipv4_hdr) + sizeof(tcp_hdr)) {
            return classifyBySequence(packet, len);  // Fallback
        }

        const tcp_hdr* tcpHeader = reinterpret_cast<const tcp_hdr*>(
            ipPacket + (ipHeader->ihl * 4));

        uint16_t srcPort = ntohs(tcpHeader->source);
        uint16_t dstPort = ntohs(tcpHeader->dest);

        // Classify based on port numbers
        // High priority: HTTP/HTTPS, SSH, DNS
        if (srcPort == 80 || dstPort == 80 || srcPort == 443 || dstPort == 443 ||
            srcPort == 22 || dstPort == 22 || srcPort == 53 || dstPort == 53) {
            return 0;  // High priority
        }
        // Medium priority: Other well-known ports
        else if (srcPort < 1024 || dstPort < 1024) {
            return 1;  // Medium-high priority
        }
        // Lower priority for ephemeral ports
        else {
            return static_cast<uint8_t>((srcPort + dstPort) % (numQueues_ - 1) + 1);
        }

    } else if (ipHeader->protocol == 17) {  // UDP
        if (len < sizeof(pdcp_hdr) + sizeof(ipv4_hdr) + sizeof(udp_hdr)) {
            return classifyBySequence(packet, len);  // Fallback
        }

        const udp_hdr* udpHeader = reinterpret_cast<const udp_hdr*>(
            ipPacket + (ipHeader->ihl * 4));

        uint16_t srcPort = ntohs(udpHeader->source);
        uint16_t dstPort = ntohs(udpHeader->dest);

        // DNS gets high priority
        if (srcPort == 53 || dstPort == 53) {
            return 0;
        }
        // RTP/media typically uses high ports, medium priority
        else if (srcPort > 1024 && dstPort > 1024) {
            return 1;
        }
        // Other UDP traffic
        else {
            return static_cast<uint8_t>((srcPort + dstPort) % numQueues_);
        }
    }

    // For other protocols, use sequence-based classification
    return classifyBySequence(packet, len);
}

uint8_t QosProfiler::classifyBySize(size_t len)
{
    // Size-based classification:
    // Small packets (< 256 bytes): High priority (likely control/signaling)
    // Medium packets (256-1024 bytes): Medium-high priority
    // Large packets (1024-1500 bytes): Medium priority
    // Very large packets (> 1500 bytes): Low priority

    if (len < 256) {
        return 0;  // High priority
    } else if (len < 1024) {
        return 1;  // Medium-high priority
    } else if (len < 1500) {
        return 2;  // Medium priority
    } else {
        return 3;  // Low priority
    }
}

uint64_t QosProfiler::getQueueStats(uint8_t queueId) const
{
    if (queueId >= numQueues_) {
        return 0;
    }
    return queueStats_[queueId];
}

void QosProfiler::resetStats()
{
    for (uint8_t i = 0; i < 4; i++) {
        queueStats_[i] = 0;
    }
    roundRobinCounter_ = 0;
    LOG_MODULE_DEBUG(MODULE, "Statistics reset");
}