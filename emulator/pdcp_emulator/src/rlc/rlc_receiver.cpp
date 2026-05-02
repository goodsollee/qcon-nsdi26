#include "rlc_receiver.hpp"
#include "pdcp_common.hpp"
#include <cstring>

// RlcReceiverPacket implementation
RlcReceiverPacket::RlcReceiverPacket(const unsigned char* buffer, size_t len) 
    : data(buffer, buffer + len), 
      size(len),
      timestamp(std::chrono::steady_clock::now()),
      packet_id(0) {}

// RlcReassemblyManager implementation
RlcReassemblyManager::RlcReassemblyManager(uint32_t timeout_ms) 
    : reassembly_timeout_ms(timeout_ms) {
    LOG_INFO("[RLC Reassembly] Manager initialized with timeout " << timeout_ms << "ms");
}

RlcReassemblyManager::~RlcReassemblyManager() {
    LOG_INFO("[RLC Reassembly] Manager cleaned up");
}

bool RlcReassemblyManager::processSegment(const RlcSegment& segment, std::vector<unsigned char>& reassembled_packet) {
    std::lock_guard<std::mutex> lock(reassembly_mutex);
    uint32_t packet_id = segment.header.original_packet_id;
    
    // Check if we already have a reassembly context for this packet
    auto it = reassembly_contexts.find(packet_id);
    if (it == reassembly_contexts.end()) {
        // Create new reassembly context
        ReassemblyContext context;
        context.original_size = segment.header.original_size;
        context.total_segments = segment.header.total_segments;
        context.segments_received = 0;
        context.last_update = std::chrono::steady_clock::now();
        context.data.resize(segment.header.original_size, 0);
        context.segment_received.resize(segment.header.total_segments, false);
        
        reassembly_contexts[packet_id] = context;
        it = reassembly_contexts.find(packet_id);
        
        LOG_DEBUG("[RLC Reassembly] Created new context for packet ID " << packet_id 
                 << " with " << context.total_segments << " segments, size " << context.original_size);
    }
    
    ReassemblyContext& context = it->second;
    uint16_t seg_num = segment.header.segment_number;
    
    // Check if segment already received
    if (context.segment_received[seg_num]) {
        LOG_DEBUG("[RLC Reassembly] Duplicate segment " << seg_num 
                 << " for packet ID " << packet_id);
        return false;
    }
    
    // Copy segment data to the right position in the reassembly buffer
    size_t offset = seg_num * (context.original_size / context.total_segments);
    if (seg_num == context.total_segments - 1) {
        // Handle last segment which might be smaller
        offset = context.original_size - segment.data.size();
    }
    
    memcpy(context.data.data() + offset, segment.data.data(), segment.data.size());
    context.segment_received[seg_num] = true;
    context.segments_received++;
    context.last_update = std::chrono::steady_clock::now();
    
    LOG_DEBUG("[RLC Reassembly] Received segment " << seg_num 
             << " for packet ID " << packet_id 
             << ", progress: " << context.segments_received << "/" << context.total_segments);
    
    // Check if all segments received
    if (context.segments_received == context.total_segments) {
        reassembled_packet = context.data;
        reassembly_contexts.erase(it);
        
        LOG_DEBUG("[RLC Reassembly] Completed packet ID " << packet_id 
                 << " with size " << reassembled_packet.size());
        return true;
    }
    
    return false;
}

void RlcReassemblyManager::cleanupExpired() {
    std::lock_guard<std::mutex> lock(reassembly_mutex);
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = reassembly_contexts.begin(); it != reassembly_contexts.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.last_update).count();
            
        if (elapsed > reassembly_timeout_ms) {
            LOG_WARN("[RLC Reassembly] Packet ID " << it->first 
                    << " timed out after " << elapsed << "ms, dropping "
                    << it->second.segments_received << "/" << it->second.total_segments << " segments");
            it = reassembly_contexts.erase(it);
        } else {
            ++it;
        }
    }
}

// RlcReceiverModule implementation
RlcReceiverModule::RlcReceiverModule(uint32_t bufferSize, uint32_t reassembly_timeout_ms) 
    : packetBuffer(bufferSize),
      maxBufferSize(bufferSize),
      packetsReassembled(0),
      packetsDequeued(0),
      packetsDropped(0),
      segmentsProcessed(0) {
    
    LOG_INFO("[RLC Receiver] Initialized with buffer size: " << bufferSize << " packets");
    
    // Initialize reassembly manager
    reassemblyManager = std::make_unique<RlcReassemblyManager>(reassembly_timeout_ms);
}

RlcReceiverModule::~RlcReceiverModule() {
    LOG_INFO("[RLC Receiver] Cleaned up. Reassembled: " << packetsReassembled.load() 
            << ", Dequeued: " << packetsDequeued.load()
            << ", Dropped: " << packetsDropped.load()
            << ", Segments processed: " << segmentsProcessed.load());
}

// Helper function to validate if a packet is likely an RLC segment
bool RlcReceiverModule::isValidSegmentHeader(const unsigned char* data) {
    // Basic sanity checks for segment header
    if (data == nullptr) return false;
    
    // Extract some fields to check for validity
    uint16_t segment_number, total_segments;
    memcpy(&segment_number, data + 4, sizeof(uint16_t));
    memcpy(&total_segments, data + 6, sizeof(uint16_t));
    
    segment_number = ntohs(segment_number);
    total_segments = ntohs(total_segments);
    
    // Basic validity check
    return (total_segments > 0 && segment_number < total_segments);
}

// Add this method to RlcReceiverModule class
bool RlcReceiverModule::encapsulateAndDeliver(const unsigned char* packet, size_t len) {
    if (!deliveryCallback) {
        LOG_WARN("[RLC Receiver] No delivery callback set, cannot deliver packet");
        return false;
    }
    
    // Check if packet is too small to encapsulate
    if (len == 0) {
        LOG_WARN("[RLC Receiver] Cannot encapsulate empty packet");
        return false;
    }
    
    // Create buffer for IP encapsulated packet
    std::vector<unsigned char> encap_buffer(sizeof(ipv4_hdr) + len);
    
    // Copy original packet to the encapsulated buffer (after IP header)
    memcpy(encap_buffer.data() + sizeof(ipv4_hdr), packet, len);
    
    // Fill in the IP header for encapsulation
    ipv4_hdr* ip_header = reinterpret_cast<ipv4_hdr*>(encap_buffer.data());
    memset(ip_header, 0, sizeof(ipv4_hdr));
    
    // Set basic IP header fields
    ip_header->version = 4;
    ip_header->ihl = 5;  // 5 * 4 = 20 bytes
    ip_header->ttl = 64;
    ip_header->protocol = 4;  // IPPROTO_IPIP (IP encapsulation)
    
    // Set fixed source and destination addresses as requested
    ip_header->saddr = htonl(0x0A640001);  // 10.100.0.1
    ip_header->daddr = htonl(0x09090909);  // 9.9.9.9
    
    // Set total length and ID
    ip_header->tot_len = htons(static_cast<uint16_t>(encap_buffer.size()));
    
    static uint16_t packet_id_counter = 0;
    ip_header->id = htons(packet_id_counter++);
    
    // Calculate IP checksum
    ip_header->check = 0;
    ip_header->check = pdcp::calculateIpChecksum(ip_header, sizeof(ipv4_hdr));
    
    // Log before delivery
    LOG_DEBUG("[RLC Receiver] Delivering encapsulated packet of size " << encap_buffer.size() << " bytes to higher layer");
    
    // Call the delivery callback with the encapsulated data
    deliveryCallback(encap_buffer.data(), encap_buffer.size());
    
    return true;
}

bool RlcReceiverModule::processSegment(const unsigned char* segment_data, size_t len) {
    if (!reassemblyManager) {
        LOG_ERROR("[RLC Receiver] No reassembly manager available");
        return false;
    }
    
    // Extract the header and validate
    if (len < 16) { // Minimum header size
        LOG_ERROR("[RLC Receiver] Segment too small to process, size: " << len);
        return false;
    }
    
    // Log detailed information about the received packet
    LOG_DEBUG("[RLC Receiver] Processing packet of size " << len << " bytes");
    
    // Deserialize the segment
    RlcSegment segment = RlcSegment::deserialize(segment_data, len);
    
    // Log segment details after deserialization
    LOG_DEBUG("[RLC Receiver] Deserialized segment - Packet ID: " << segment.header.original_packet_id
             << ", Segment: " << segment.header.segment_number + 1
             << "/" << segment.header.total_segments
             << ", Size: " << segment.header.segment_size);
    
    segmentsProcessed.fetch_add(1, std::memory_order_relaxed);
    
    // Check if this is a non-segmented packet (total_segments == 1)
    if (segment.header.total_segments == 1 && segment.header.segment_number == 0) {
        LOG_DEBUG("[RLC Receiver] Processing non-segmented packet with ID: " << segment.header.original_packet_id);
        
        // For non-segmented packets, we can directly enqueue the data portion
        if (encapsulateAndDeliver(segment.data.data(), segment.data.size())) {
            packetsReassembled.fetch_add(1, std::memory_order_relaxed);
            return true;
        } else {
            packetsDropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[RLC Receiver] Failed to deliver non-segmented packet");
            return false;
        }
    }
    
    // For segmented packets, use the reassembly process
    std::vector<unsigned char> reassembled;
    bool packet_complete = reassemblyManager->processSegment(segment, reassembled);
    
    // If a packet is completely reassembled, enqueue it
    if (packet_complete && !reassembled.empty()) {
        LOG_DEBUG("[RLC Receiver] Reassembled complete packet of size " << reassembled.size() << " bytes");
        packetsReassembled.fetch_add(1, std::memory_order_relaxed);
        
        if (encapsulateAndDeliver(reassembled.data(), reassembled.size())) {
            packetsReassembled.fetch_add(1, std::memory_order_relaxed);
            return true;
        } else {
            packetsDropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[RLC Receiver] Failed to deliver non-segmented packet");
            return false;
        }
    }
    
    // Clean up any expired reassembly contexts
    reassemblyManager->cleanupExpired();
    return true;
}

bool RlcReceiverModule::enqueueReassembledPacket(const unsigned char* packet, size_t len) {
    // Create new RLC packet
    auto rlcPacket = std::make_shared<RlcReceiverPacket>(packet, len);
    
    // Try to enqueue to the SPSC queue
    if (!packetBuffer.try_enqueue(rlcPacket)) {
        LOG_WARN("[RLC Receiver] Buffer full, reassembled packet dropped");
        return false;
    }
    
    LOG_DEBUG("[RLC Receiver] Reassembled packet enqueued, size: " << len 
             << " bytes, buffer estimated size: " << getBufferOccupancy());
    return true;
}

std::shared_ptr<RlcReceiverPacket> RlcReceiverModule::dequeuePacket(size_t maxSize) {
    auto* peekedPtr = packetBuffer.peek();
    // Try to peek at the front packet without removing it
    if (peekedPtr == nullptr) {
        return nullptr; // Queue is empty
    }
    
    std::shared_ptr<RlcReceiverPacket> packet = *peekedPtr;

    // Check if front packet fits within available bytes
    if (packet->size <= maxSize) {
        // Remove the packet from the queue
        packetBuffer.try_dequeue(packet);
        packetsDequeued.fetch_add(1, std::memory_order_relaxed);
        LOG_DEBUG("[RLC Receiver] Packet dequeued, size: " << packet->size << " bytes");
        return packet;
    }
    
    // Packet doesn't fit, cannot dequeue
    LOG_DEBUG("[RLC Receiver] Front packet too large for available bandwidth: " 
            << packet->size << " > " << maxSize);
    return nullptr;
}

bool RlcReceiverModule::hasPackets() const {
    return packetBuffer.size_approx() > 0;
}

size_t RlcReceiverModule::getBufferOccupancy() const {
    return packetBuffer.size_approx();
}

size_t RlcReceiverModule::getBufferCapacity() const {
    return maxBufferSize;
}

void RlcReceiverModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
    
    LOG_INFO("[RLC Receiver] Delivery callback set");
}

void RlcReceiverModule::getStats(uint32_t& reassembled, uint32_t& dequeued, 
                                 uint32_t& dropped, uint32_t& segments_processed) const {
    reassembled = packetsReassembled.load(std::memory_order_relaxed);
    dequeued = packetsDequeued.load(std::memory_order_relaxed);
    dropped = packetsDropped.load(std::memory_order_relaxed);
    segments_processed = segmentsProcessed.load(std::memory_order_relaxed);
}