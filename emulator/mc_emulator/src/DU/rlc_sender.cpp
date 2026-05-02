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

std::vector<std::shared_ptr<RlcSegment>> 
RlcSegmentationManager::segmentPacket(const unsigned char* packet, size_t length, size_t max_segment_size, uint32_t packet_id)
{
    size_t max_payload_size = (max_segment_size > RLC_SEGMENT_HEADER_SIZE)
                            ? (max_segment_size - RLC_SEGMENT_HEADER_SIZE)
                            : 0;
    if (max_payload_size == 0) {
        LOG_ERROR("[RLC Segmentation] Max segment size too small for header");
        return {};
    }

    uint16_t total_segments = (length + max_payload_size - 1) / max_payload_size;

    LOG_DEBUG("[RLC Segmentation] Segmenting packet ID " << packet_id 
             << " of size " << length << " into " << total_segments << " segments");

    std::vector<std::shared_ptr<RlcSegment>> segments;
    segments.reserve(total_segments);

    for (uint16_t i = 0; i < total_segments; i++) {
        size_t offset    = i * max_payload_size;
        size_t remaining = length - offset;
        size_t seg_size  = (remaining > max_payload_size)
                           ? max_payload_size
                           : remaining;
        bool is_last = (i == total_segments - 1);

        auto segment = std::make_shared<RlcSegment>(
            packet_id,
            i,
            total_segments,
            length,
            is_last,
            packet + offset,
            seg_size
        );

        segment->header.offset_in_original = static_cast<uint32_t>(offset);

        segments.push_back(segment);

        LOG_DEBUG("[RLC Segmentation] Created segment " << i+1 << "/" << total_segments
                  << " of size " << seg_size << " bytes, offset=" << offset);
    }

    return segments;
}

// RlcSenderModule implementation
RlcSenderModule::RlcSenderModule(uint32_t bufferSize, 
                                uint32_t retransmission_timeout_ms,
                                uint16_t max_retries,
                                uint32_t retransmit_timer,
                                const std::string& log_dir) 
    : packetBuffer(bufferSize),
      segmentBuffer(bufferSize * 4), // Segments buffer can be larger
      maxBufferSize(bufferSize),
      packetsDropped(0),
      packetsBuffered(0),
      packetsDequeued(0),
      segmentsCreated(0),
      segmentsDequeued(0),
      next_packet_id(1),
      maxSegmentSize(RLC_MAX_SEGMENT_SIZE),
      t_pollRetransmit_ms(retransmit_timer) {
    
    LOG_INFO("[RLC Sender] Initialized with buffer size: " << bufferSize << " packets");

    t_pollRetransmit_expiry = std::chrono::steady_clock::now() + 
                             std::chrono::milliseconds(t_pollRetransmit_ms);
    
    // Initialize segmentation manager
    segmentationManager = std::make_unique<RlcSegmentationManager>();

    // Initialize retransmission manager with log directory
    retransmissionManager = std::make_unique<RlcRetransmissionManager>(
        retransmission_timeout_ms, max_retries, log_dir);
}

RlcSenderModule::~RlcSenderModule() {
    uint32_t acked, retransmitted, dropped;
    double avg_e2e_delay = 0.0, avg_queue_delay = 0.0, avg_ack_delay = 0.0;
    uint32_t bytes_acked = 0;
    
    if (retransmissionManager) {
        retransmissionManager->getStats(acked, bytes_acked, retransmitted, dropped, 
                                      avg_e2e_delay, avg_queue_delay, avg_ack_delay);
        
        LOG_INFO("[RLC Sender] Cleaned up. Buffered: " << packetsBuffered.load() 
                << ", Dequeued: " << packetsDequeued.load()
                << ", Dropped: " << packetsDropped.load()
                << ", Segments created: " << segmentsCreated.load()
                << ", Segments dequeued: " << segmentsDequeued.load()
                << ", ACKed: " << acked
                << ", Bytes ACKed: " << bytes_acked
                << ", Retransmitted: " << retransmitted
                << ", Retransmission dropped: " << dropped);
        
        LOG_INFO("[RLC Sender] Average latency (ms) - End-to-End: " << avg_e2e_delay
                << ", Queueing: " << avg_queue_delay
                << ", ACK: " << avg_ack_delay);
    } else {
        LOG_INFO("[RLC Sender] Cleaned up. Buffered: " << packetsBuffered.load() 
                << ", Dequeued: " << packetsDequeued.load()
                << ", Dropped: " << packetsDropped.load()
                << ", Segments created: " << segmentsCreated.load()
                << ", Segments dequeued: " << segmentsDequeued.load());
    }
}

bool RlcSenderModule::enqueuePacket(const unsigned char* packet, size_t len) {
    return enqueuePacket(packet, len, 0, 0);  // Default to queue 0, priority 0
}

bool RlcSenderModule::enqueuePacket(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority) {

    // Check if we need to perform IP decapsulation first
    if (len < sizeof(ipv4_hdr)) {
        LOG_WARN("[RLC Sender] Packet too small for IP header");
        return false;
    }

    LOG_DEBUG("[RLC Sender] Processing packet of size " << len << " bytes");

    // Create new RLC packet
    auto rlcPacket = std::make_shared<RlcPacket>(packet, len);

    // Set queue metadata
    rlcPacket->queue_id = queue_id;
    rlcPacket->priority = priority;

    // Try to enqueue to the SPSC queue
    if (!packetBuffer.try_enqueue(rlcPacket)) {
        packetsDropped.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("[RLC Sender] Buffer full, packet dropped");
        return false;
    }

    rlcPacket->packet_id = (next_packet_id++ & RLC_SEQ_NUM_MASK);

    packetsBuffered.fetch_add(1, std::memory_order_relaxed);
    totalBufferedBytes.fetch_add(len, std::memory_order_relaxed);
    LOG_DEBUG("[RLC Sender] Packet enqueued, ID: " << rlcPacket->packet_id
             << ", queue_id: " << static_cast<int>(queue_id)
             << ", priority: " << static_cast<int>(priority)
             << ", size: " << len << " bytes, buffer estimated size: " << getBufferOccupancy());

    // Add packet to retransmission buffer
    if (retransmissionManager) {
        retransmissionManager->addPacket(rlcPacket->packet_id, len);
    }

    return true;
}

std::shared_ptr<RlcPacket> RlcSenderModule::dequeuePacket(size_t availableBytes) {
    // Check poll timer first
    checkPollTimer();
    
    // First check if we have any segments ready to send
    auto* peekedSegmentPtr = segmentBuffer.peek();
    
    LOG_DEBUG("[RLC Sender] Available bandwidth: " << availableBytes << " bytes, buffer size: "<< getBufferOccupancy());
    if (peekedSegmentPtr != nullptr) {
        std::shared_ptr<RlcSegment> segment = *peekedSegmentPtr;
        size_t segment_size = segment->serialize().size();
        
        if (segment_size <= availableBytes) {
            // Dequeue the segment
            segmentBuffer.try_dequeue(segment);
            segmentsDequeued.fetch_add(1, std::memory_order_relaxed);
            totalSegmentBufferedBytes.fetch_sub(segment->data.size(), std::memory_order_relaxed);
            
            // Check if we should set poll bit (for last segment of a packet)
            {
                std::lock_guard<std::mutex> lock(timer_mutex);
                if (poll_bit_pending && segment->header.is_last_segment) {
                    // Set poll bit in the segment header
                    segment->header.poll = true;
                    poll_bit_pending = false;
                    
                    // Restart timer
                    t_pollRetransmit_expiry = std::chrono::steady_clock::now() + 
                                             std::chrono::milliseconds(t_pollRetransmit_ms);
                    
                    LOG_DEBUG("[RLC Sender] Poll bit set on last segment, ID: " 
                             << segment->header.original_packet_id
                             << ", resetting t_pollRetransmit");
                }
            }
            
            // Record segment dequeue in retransmission manager
            if (retransmissionManager) {
                retransmissionManager->recordSegmentDequeued(
                    segment->header.original_packet_id,
                    segment->header.segment_number,
                    segment->header.total_segments,
                    segment->data.size()
                );
            }
            
            // Create a packet from the segment
            auto serialized = segment->serialize();
            auto packet = std::make_shared<RlcPacket>(serialized.data(), serialized.size());
            
            // Set the poll flag in the RlcPacket based on the segment's poll bit
            packet->poll = segment->header.poll;
            
            LOG_DEBUG("[RLC Sender] Segment dequeued, ID: " << segment->header.original_packet_id
                     << ", segment: " << segment->header.segment_number + 1 
                     << "/" << segment->header.total_segments
                     << ", size: " << segment_size 
                     << ", poll: " << (packet->poll ? "true" : "false")
                     << " bytes");
            
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
        totalBufferedBytes.fetch_sub(packet->size, std::memory_order_relaxed);
        
        // Check if we should set poll bit for this packet
        bool set_poll_bit = false;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            if (poll_bit_pending) {
                set_poll_bit = true;
                poll_bit_pending = false;
                
                // Restart timer
                t_pollRetransmit_expiry = std::chrono::steady_clock::now() + 
                                         std::chrono::milliseconds(t_pollRetransmit_ms);
                
                LOG_DEBUG("[RLC Sender] Poll bit will be set on non-segmented packet, ID: " 
                         << packet->packet_id << ", resetting t_pollRetransmit");
            }
        }
        
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

        // Set poll bit if needed
        if (set_poll_bit) {
            segment->header.poll = true;
        }

        // Propagate queue information from packet to segment
        segment->header.queue_id = packet->queue_id;
        segment->header.priority = packet->priority;
        
        // Record segment dequeue in retransmission manager
        if (retransmissionManager) {
            retransmissionManager->recordSegmentDequeued(
                packet->packet_id,
                0,  // segment 0
                1,  // total segments is 1
                packet->size
            );
        }
        
        // Serialize the segment (adds header)
        auto serialized = segment->serialize();
        
        // Create a new packet with header included
        auto headerized_packet = std::make_shared<RlcPacket>(serialized.data(), serialized.size());
        
        // Set the poll flag in the RlcPacket
        headerized_packet->poll = set_poll_bit;
        
        LOG_DEBUG("[RLC Sender] Non-segmented packet with header, ID: " 
                 << packet->packet_id << ", original size: " << packet->size 
                 << ", with header: " << serialized.size() 
                 << ", poll: " << (set_poll_bit ? "true" : "false")
                 << " bytes");
        
        return headerized_packet;
    }
    
    // Packet doesn't fit, need to segment it
    LOG_DEBUG("[RLC Sender] Segmenting packet ID: " << packet->packet_id 
             << ", size: " << packet->size << " bytes");
    
    // Calculate effective max segment size (adjusting for header overhead)
    size_t effective_max_segment = std::min(maxSegmentSize, availableBytes);
    if (effective_max_segment < 100) {
        // Minimum reasonable segment size with header
        LOG_WARN("[RLC Sender] Available bytes too small for segmentation: " << availableBytes << " max: " << maxSegmentSize);
        return nullptr;
    }

    LOG_DEBUG("[RLC Sender] Attempting to segment packet ID: " << packet->packet_id 
            << " with effective_max_segment: " << effective_max_segment);
    
    // Segment the packet
    auto segments = segmentationManager->segmentPacket(
        packet->data.data(), packet->size, effective_max_segment, packet->packet_id);

    LOG_DEBUG("[RLC Sender] segmentPacket returned " << segments.size() << " segments");
    
    if (segments.empty()) {
        LOG_ERROR("[RLC Sender] Failed to segment packet");
        return nullptr;
    }
    
    // Check if we should set poll bit on the last segment
    bool set_poll_bit = false;
    {
        std::lock_guard<std::mutex> lock(timer_mutex);
        if (poll_bit_pending) {
            set_poll_bit = true;
            poll_bit_pending = false;
            
            // Restart timer
            t_pollRetransmit_expiry = std::chrono::steady_clock::now() + 
                                     std::chrono::milliseconds(t_pollRetransmit_ms);
            
            LOG_DEBUG("[RLC Sender] Poll bit will be set on last segment of packet ID: " 
                     << packet->packet_id << ", resetting t_pollRetransmit");
        }
    }
    
    // Set poll bit on the last segment if needed
    if (set_poll_bit && !segments.empty()) {
        // Find the last segment
        for (auto& segment : segments) {
            if (segment->header.is_last_segment) {
                segment->header.poll = true;
                LOG_DEBUG("[RLC Sender] Poll bit set on last segment of packet ID: " 
                         << packet->packet_id);
                break;
            }
        }
    }
    
    // Queue all segments
    for (auto& segment : segments) {
        // Propagate queue information from packet to segments
        segment->header.queue_id = packet->queue_id;
        segment->header.priority = packet->priority;

        if (!segmentBuffer.try_enqueue(segment)) {
            LOG_WARN("[RLC Sender] Segment buffer full, segment dropped");
            // Continue trying to enqueue the rest
        } else {
            segmentsCreated.fetch_add(1, std::memory_order_relaxed);
            totalSegmentBufferedBytes.fetch_add(segment->data.size(), std::memory_order_relaxed);
        }
    }

    LOG_DEBUG("[RLC Sender] Successfully enqueued " << segments.size() << " segments");
    
    // Remove the original packet from the queue (This is used when packet overflows! why?)
    packetBuffer.try_dequeue(packet);
    packetsDequeued.fetch_add(1, std::memory_order_relaxed);
    totalBufferedBytes.fetch_sub(packet->size, std::memory_order_relaxed);
    
    LOG_DEBUG("[RLC Sender] Packet segmented into " << segments.size() 
             << " segments, queued for transmission");
    
    // Now try to dequeue a segment that fits
    return dequeuePacket(availableBytes);
}

// Process an incoming ACK
bool RlcSenderModule::processAck(const unsigned char* ack_data, size_t len) {
    if (!retransmissionManager) {
        LOG_WARN("[RLC Sender] No retransmission manager available");
        return false;
    }

    // Check if this is a Status PDU
    if (len >= 1 && ack_data[0] == RLC_PDU_TYPE_STATUS) {
        if (len < 12) {
            LOG_ERROR("[RLC Sender] Status PDU too small: " << len);
            return false;
        }
        
        // Deserialize the Status PDU
        RlcStatusPdu status_pdu = RlcStatusPdu::deserialize(ack_data, len);
        
        LOG_DEBUG("[RLC Sender] Received Status PDU with ACK SN: " << status_pdu.ack_sn
                 << ", control: 0x" << std::hex << (int)status_pdu.control << std::dec
                 << ", window: " << status_pdu.window_size);

        std::lock_guard<std::mutex> lock(timer_mutex);
        t_pollRetransmit_expiry = std::chrono::steady_clock::now() + 
                                    std::chrono::milliseconds(t_pollRetransmit_ms);
        poll_bit_pending = false;

        LOG_DEBUG("[RLC Sender] Status PDU received, resetting t_pollRetransmit");
        
        // Process in retransmission manager
        return retransmissionManager->processStatusPdu(status_pdu);
    } 
    
    // Legacy ACK processing is no longer supported
    LOG_WARN("[RLC Sender] Received unknown ACK format - only Status PDUs are supported");
    return false;
}

// Add this method to RlcSenderModule
// Add this method to RlcSenderModule
void RlcSenderModule::checkPollTimer() {
    std::lock_guard<std::mutex> lock(timer_mutex);
    auto now = std::chrono::steady_clock::now();
    
    // Add debug info to see current time vs expiry time
    auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_pollRetransmit_expiry - now).count();
    
    LOG_DEBUG("[RLC Sender] Timer check - Current time diff from expiry: " 
             << time_diff << "ms, poll_bit_pending=" 
             << (poll_bit_pending ? "true" : "false"));
    
    if (time_diff < 0) {
        poll_bit_pending = true;
        LOG_DEBUG("[RLC Sender] t_pollRetransmit expired, next packet will have poll bit set");
    }
}

// Handle retransmissions
void RlcSenderModule::checkRetransmissions() {
    if (!retransmissionManager) {
        return;
    }
    
    std::vector<uint32_t> retransmissions = retransmissionManager->checkForRetransmissions();
    if (retransmissions.empty() || !retransmissionCallback) {
        return;
    }
    
    for (uint32_t packet_id : retransmissions) {
        if (retransmissionCallback(packet_id)) {
            LOG_DEBUG("[RLC Sender] Retransmission callback triggered for packet ID " << packet_id);
            retransmissionManager->markRetransmitted(packet_id);
        }
    }
}

// Set callback for retransmitting packets
void RlcSenderModule::setRetransmissionCallback(std::function<bool(uint32_t)> callback) {
    retransmissionCallback = callback;
    LOG_INFO("[RLC Sender] Retransmission callback set");
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

// Implement the buffer size methods
size_t RlcSenderModule::getBufferSizeBytes() const {
    return totalBufferedBytes.load(std::memory_order_relaxed);
}

size_t RlcSenderModule::getSegmentBufferSizeBytes() const {
    return totalSegmentBufferedBytes.load(std::memory_order_relaxed);
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