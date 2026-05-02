#include "rlc_sender.hpp"
#include "pdcp_common.hpp"

// RlcPacket implementation
RlcPacket::RlcPacket(const unsigned char* buffer, size_t len) 
    : data(buffer, buffer + len), 
      size(len),
      timestamp(std::chrono::steady_clock::now()),
      packet_id(0) {}

// RlcSegmentationManager implementation
RlcSegmentationManager::RlcSegmentationManager() 
    : next_packet_id(1) {
    LOG_INFO("[RLC Segmentation] Manager initialized");
}

RlcSegmentationManager::~RlcSegmentationManager() {
    LOG_INFO("[RLC Segmentation] Manager cleaned up");
}

std::vector<std::shared_ptr<RlcSegment>> RlcSegmentationManager::segmentPacket(
    const unsigned char* packet, size_t length, size_t max_segment_size) {
    
    // Ensure max_segment_size accounts for the segment header
    size_t max_payload_size = (max_segment_size > 16) ? max_segment_size - 16 : 0;
    if (max_payload_size == 0) {
        LOG_ERROR("[RLC Segmentation] Max segment size too small for header");
        return {};
    }
    
    // Calculate number of segments needed
    uint16_t total_segments = (length + max_payload_size - 1) / max_payload_size;
    uint32_t packet_id = next_packet_id++;
    
    std::vector<std::shared_ptr<RlcSegment>> segments;
    
    LOG_DEBUG("[RLC Segmentation] Segmenting packet ID " << packet_id 
             << " of size " << length << " into " << total_segments << " segments");
    
    for (uint16_t i = 0; i < total_segments; i++) {
        // Calculate segment data size
        size_t offset = i * max_payload_size;
        size_t remaining = length - offset;
        size_t segment_size = (remaining > max_payload_size) ? max_payload_size : remaining;
        bool is_last = (i == total_segments - 1);
        
        // Create segment
        auto segment = std::make_shared<RlcSegment>(
            packet_id, i, total_segments, length, is_last,
            packet + offset, segment_size
        );
        
        segments.push_back(segment);
        
        LOG_DEBUG("[RLC Segmentation] Created segment " << i+1 << "/" << total_segments 
                  << " of size " << segment_size << " bytes");
    }
    
    return segments;
}

// RlcSenderModule implementation
RlcSenderModule::RlcSenderModule(uint32_t bufferSize) 
    : packetBuffer(bufferSize),
      segmentBuffer(bufferSize * 4), // Segments buffer can be larger
      maxBufferSize(bufferSize),
      packetsDropped(0),
      packetsBuffered(0),
      packetsDequeued(0),
      segmentsCreated(0),
      segmentsDequeued(0),
      next_packet_id(1),
      maxSegmentSize(RLC_MAX_SEGMENT_SIZE) {
    
    LOG_INFO("[RLC Sender] Initialized with buffer size: " << bufferSize << " packets");
    
    // Initialize segmentation manager
    segmentationManager = std::make_unique<RlcSegmentationManager>();
}

RlcSenderModule::~RlcSenderModule() {
    LOG_INFO("[RLC Sender] Cleaned up. Buffered: " << packetsBuffered.load() 
            << ", Dequeued: " << packetsDequeued.load()
            << ", Dropped: " << packetsDropped.load()
            << ", Segments created: " << segmentsCreated.load()
            << ", Segments dequeued: " << segmentsDequeued.load());
}

bool RlcSenderModule::enqueuePacket(const unsigned char* packet, size_t len) {

    // Check if we need to perform IP decapsulation first
    if (len < sizeof(ipv4_hdr)) {
        LOG_WARN("[RLC Sender] Packet too small for IP header");
        return false;
    }

    // Check if this is an IP-in-IP packet
    const ipv4_hdr* ip_header = reinterpret_cast<const ipv4_hdr*>(packet);
    bool is_ip_encapsulated = (ip_header->version == 4 && ip_header->protocol == 4); // IPPROTO_IPIP = 4
    
    if (!is_ip_encapsulated) {
        LOG_DEBUG("[RLC Sender] Packet is not IP-encapsulated, processing as is");
        // Not an IP-encapsulated packet, process as is
    } else {
        // IP-encapsulated packet, perform decapsulation
        size_t ip_header_len = ip_header->ihl * 4; // IHL is in 32-bit words
        
        if (len <= ip_header_len) {
            LOG_WARN("[RLC Sender] Invalid IP header length");
            return false;
        }
        
        // Skip outer IP header, only use inner packet (PDCP + inner IP)
        packet += ip_header_len;
        len -= ip_header_len;
        
        LOG_DEBUG("[RLC Sender] Decapsulated outer IP header, remaining length: " << len);
    }

    // Create new RLC packet
    auto rlcPacket = std::make_shared<RlcPacket>(packet, len);
    rlcPacket->packet_id = next_packet_id++;
    
    // Try to enqueue to the SPSC queue
    if (!packetBuffer.try_enqueue(rlcPacket)) {
        packetsDropped.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("[RLC Sender] Buffer full, packet dropped");
        return false;
    }
    
    packetsBuffered.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("[RLC Sender] Packet enqueued, ID: " << rlcPacket->packet_id 
             << ", size: " << len << " bytes, buffer estimated size: " << getBufferOccupancy());
    return true;
}

std::shared_ptr<RlcPacket> RlcSenderModule::dequeuePacket(size_t availableBytes) {
    // First check if we have any segments ready to send
    auto* peekedSegmentPtr = segmentBuffer.peek();
    if (peekedSegmentPtr != nullptr) {
        std::shared_ptr<RlcSegment> segment = *peekedSegmentPtr;
        size_t segment_size = segment->serialize().size();
        
        if (segment_size <= availableBytes) {
            // Dequeue the segment
            segmentBuffer.try_dequeue(segment);
            segmentsDequeued.fetch_add(1, std::memory_order_relaxed);
            
            // Create a packet from the segment
            auto serialized = segment->serialize();
            auto packet = std::make_shared<RlcPacket>(serialized.data(), serialized.size());
            
            LOG_DEBUG("[RLC Sender] Segment dequeued, ID: " << segment->header.original_packet_id
                     << ", segment: " << segment->header.segment_number + 1 
                     << "/" << segment->header.total_segments
                     << ", size: " << segment_size << " bytes");
            
            return packet;
        } else {
            LOG_DEBUG("[RLC Sender] Front segment too large for available bandwidth: " 
                    << segment_size << " > " << availableBytes);
            return nullptr;
        }
    }
    
    // If no segments available, check for packets to segment
    auto* peekedPacketPtr = packetBuffer.peek();
    if (peekedPacketPtr == nullptr) {
        return nullptr; // Queue is empty
    }
    
    std::shared_ptr<RlcPacket> packet = *peekedPacketPtr;
    
    // If packet fits directly, send it with a single segment header
    if (packet->size + 16 <= availableBytes) {  // +16 for header size
        // Remove the packet from the queue
        packetBuffer.try_dequeue(packet);
        packetsDequeued.fetch_add(1, std::memory_order_relaxed);
        
        // Create a "segment" for the entire packet
        auto segment = std::make_shared<RlcSegment>(
            packet->packet_id,    // Original packet ID
            0,                    // Segment number (0 for non-segmented)
            1,                    // Total segments (1 for non-segmented)
            packet->size,         // Original size
            true,                 // Is last segment
            packet->data.data(),  // Packet data 
            packet->size          // Data length
        );
        
        // Serialize the segment (adds header)
        auto serialized = segment->serialize();
        
        // Create a new packet with header included
        auto headerized_packet = std::make_shared<RlcPacket>(serialized.data(), serialized.size());
        
        LOG_DEBUG("[RLC Sender] Non-segmented packet with header, ID: " 
                 << packet->packet_id << ", original size: " << packet->size 
                 << ", with header: " << serialized.size() << " bytes");
        
        return headerized_packet;
    }
    
    // Packet doesn't fit, need to segment it
    LOG_DEBUG("[RLC Sender] Segmenting packet ID: " << packet->packet_id 
             << ", size: " << packet->size << " bytes");
    
    // Calculate effective max segment size (adjusting for header overhead)
    size_t effective_max_segment = std::min(maxSegmentSize, availableBytes);
    if (effective_max_segment < 100) {
        // Minimum reasonable segment size with header
        LOG_WARN("[RLC Sender] Available bytes too small for segmentation: " << availableBytes);
        return nullptr;
    }

    LOG_DEBUG("[RLC Sender] Attempting to segment packet ID: " << packet->packet_id 
            << " with effective_max_segment: " << effective_max_segment);
    
    // Segment the packet
    auto segments = segmentationManager->segmentPacket(
        packet->data.data(), packet->size, effective_max_segment);

    LOG_DEBUG("[RLC Sender] segmentPacket returned " << segments.size() << " segments");
    
    if (segments.empty()) {
        LOG_ERROR("[RLC Sender] Failed to segment packet");
        return nullptr;
    }
    
    // Queue all segments
    for (auto& segment : segments) {
        if (!segmentBuffer.try_enqueue(segment)) {
            LOG_WARN("[RLC Sender] Segment buffer full, segment dropped");
            // Continue trying to enqueue the rest
        } else {
            segmentsCreated.fetch_add(1, std::memory_order_relaxed);
        }
    }

    LOG_DEBUG("[RLC Sender] Successfully enqueued " << segments.size() << " segments");
    
    // Remove the original packet from the queue
    packetBuffer.try_dequeue(packet);
    packetsDequeued.fetch_add(1, std::memory_order_relaxed);
    
    LOG_DEBUG("[RLC Sender] Packet segmented into " << segments.size() 
             << " segments, queued for transmission");
    
    // Now try to dequeue a segment that fits
    return dequeuePacket(availableBytes);
}

void RlcSenderModule::setMaxSegmentSize(size_t size) {
    if (size > 100) { // Minimum reasonable size
        maxSegmentSize = size;
        LOG_INFO("[RLC Sender] Max segment size set to " << size << " bytes");
    }
}

bool RlcSenderModule::hasPackets() const {
    return packetBuffer.size_approx() > 0 || segmentBuffer.size_approx() > 0;
}

size_t RlcSenderModule::getBufferOccupancy() const {
    return packetBuffer.size_approx();
}

size_t RlcSenderModule::getSegmentBufferOccupancy() const {
    return segmentBuffer.size_approx();
}

size_t RlcSenderModule::getFrontPacketSize() const {
    // First check segments
    auto* peekedSegmentPtr = segmentBuffer.peek();
    if (peekedSegmentPtr != nullptr) {
        return (*peekedSegmentPtr)->serialize().size();
    }
    
    // Then check regular packets
    auto* peekedPacketPtr = packetBuffer.peek();
    if (peekedPacketPtr == nullptr) {
        return 0;
    }
    
    // If packet is larger than max segment size, return segment size
    if ((*peekedPacketPtr)->size > maxSegmentSize) {
        return maxSegmentSize;
    }
    
    return (*peekedPacketPtr)->size;
}

size_t RlcSenderModule::getBufferCapacity() const {
    return maxBufferSize;
}

void RlcSenderModule::getStats(uint32_t& buffered, uint32_t& dequeued, uint32_t& dropped,
                               uint32_t& segments_created, uint32_t& segments_dequeued) const {
    buffered = packetsBuffered.load(std::memory_order_relaxed);
    dequeued = packetsDequeued.load(std::memory_order_relaxed);
    dropped = packetsDropped.load(std::memory_order_relaxed);
    segments_created = segmentsCreated.load(std::memory_order_relaxed);
    segments_dequeued = segmentsDequeued.load(std::memory_order_relaxed);
}