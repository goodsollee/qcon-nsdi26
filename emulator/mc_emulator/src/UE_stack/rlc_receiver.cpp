#include "rlc_receiver.hpp"
#include "pdcp_common.hpp"
#include <cstring>
#include <algorithm>



// RlcReassemblyManager implementation
RlcReassemblyManager::RlcReassemblyManager(uint32_t timeout_ms) 
    : reassembly_timeout_ms(timeout_ms) {
    LOG_INFO("[RLC Reassembly] Manager initialized with timeout " << timeout_ms << "ms");
}

RlcReassemblyManager::~RlcReassemblyManager() {
    LOG_INFO("[RLC Reassembly] Manager cleaned up");
}

bool RlcReassemblyManager::processSegment(const RlcSegment& segment, 
                                          std::vector<unsigned char>& reassembled_packet)
{
    std::lock_guard<std::mutex> lock(reassembly_mutex);
    uint32_t packet_id = segment.header.original_packet_id & RLC_SEQ_NUM_MASK;

    // Check if we have too many contexts
    if (reassembly_contexts.size() >= RLC_MAX_REASSEMBLY_CONTEXTS && 
        reassembly_contexts.find(packet_id) == reassembly_contexts.end()) {
        LOG_WARN("[RLC Reassembly] Too many reassembly contexts, dropping new segment for packet ID " << packet_id);
        return false;
    }

    auto it = reassembly_contexts.find(packet_id);
    if (it == reassembly_contexts.end()) {
        ReassemblyContext ctx;
        ctx.original_size       = segment.header.original_size;
        ctx.total_segments      = segment.header.total_segments;
        ctx.segments_received   = 0;
        ctx.last_update         = std::chrono::steady_clock::now();

        ctx.data.resize(ctx.original_size, 0);
        ctx.segment_received.resize(ctx.total_segments, false);

        reassembly_contexts[packet_id] = ctx;
        it = reassembly_contexts.find(packet_id);

        LOG_DEBUG("[RLC Reassembly] Created new context for packet ID " << packet_id
                  << " with " << ctx.total_segments 
                  << " segments, size " << ctx.original_size);
    }

    ReassemblyContext& context = it->second;
    uint16_t seg_num = segment.header.segment_number;

    if (seg_num >= context.segment_received.size()) {
        LOG_WARN("[RLC Reassembly] Invalid segment number " << seg_num 
                 << " for packet ID " << packet_id);
        return false;
    }

    if (context.segment_received[seg_num]) {
        LOG_DEBUG("[RLC Reassembly] Duplicate segment " << seg_num 
                  << " for packet ID " << packet_id);
        return false;
    }

    size_t offset = segment.header.offset_in_original;
    if (offset + segment.data.size() > context.data.size()) {
        LOG_WARN("[RLC Reassembly] Segment overflow: offset=" << offset 
                 << " seg_size=" << segment.data.size() 
                 << " original_size=" << context.data.size());
        return false;
    }

    memcpy(context.data.data() + offset, 
           segment.data.data(), 
           segment.data.size());

    context.segment_received[seg_num] = true;
    context.segments_received++;
    context.last_update = std::chrono::steady_clock::now();

    LOG_DEBUG("[RLC Reassembly] Received segment " << seg_num 
              << " for packet ID " << packet_id
              << ", progress: " << context.segments_received 
              << "/" << context.total_segments);

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
RlcReceiverModule::RlcReceiverModule(const RlcReceiverConfig& cfg, const std::string& log_dir) 
    : packetBuffer(cfg.bufferSize),
      maxBufferSize(cfg.bufferSize),
      packetsReassembled(0),
      packetsDequeued(0),
      packetsDropped(0),
      segmentsProcessed(0),
      acksSent(0),
      current_window_size(cfg.bufferSize),
      status_pdu_thread_running(false),
      status_pdu_interval_ms(cfg.status_pdu_interval_ms),
      highest_sn_seen(0),
      last_status_report_time(std::chrono::steady_clock::now()),
      pending_poll(false),
      t_statusProhibit_ms(cfg.t_statusProhibit_ms)  {
    
    LOG_INFO("[RLC Receiver] Configuration:"
             << " bufferSize=" << cfg.bufferSize
             << " reassembly_timeout_ms=" << cfg.reassembly_timeout_ms
             << " status_pdu_interval_ms=" << cfg.status_pdu_interval_ms
             << " t_statusProhibit_ms=" << cfg.t_statusProhibit_ms
             << " max_retransmissions=" << static_cast<int>(cfg.max_retransmissions));
    
    // Initialize reassembly manager
    reassemblyManager = std::make_unique<RlcReassemblyManager>(cfg.reassembly_timeout_ms);
    
    // Initialize logger for status PDU transmissions
    // Create log directory if it doesn't exist
    std::string mkdir_cmd = "mkdir -p " + log_dir;
    int result = system(mkdir_cmd.c_str());
    if (result != 0) {
        LOG_WARN("[RLC Receiver] Failed to create log directory: " << log_dir);
    }
    
    // Create logger with timestamp in filename
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&time_now, &tm_now);
    
    std::ostringstream oss;
    oss << log_dir << "/rlc_status_pdu_" 
        << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".csv";
    
    std::string log_path = oss.str();
    status_pdu_logger = std::make_unique<AsyncLogger>(log_path);
    
    // Write CSV header
    status_pdu_logger->logLine("timestamp,status_pdu_count,waiting_queue_size");
    LOG_INFO("[RLC Receiver] Status PDU logging to " << log_path);

    LOG_INFO("[RLC Receiver] RLC Receiver Module initialized");

    // Start the status PDU thread
    startStatusPduThread();
}

RlcReceiverModule::~RlcReceiverModule() {
    // Stop the status PDU thread if it's running
    stopStatusPduThread();
    
    LOG_INFO("[RLC Receiver] Cleaned up. Reassembled: " << packetsReassembled.load() 
            << ", Dequeued: " << packetsDequeued.load()
            << ", Dropped: " << packetsDropped.load()
            << ", Segments processed: " << segmentsProcessed.load()
            << ", ACKs sent: " << acksSent.load());
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
    return encapsulateAndDeliver(packet, len, 0, 0); // Default to queue 0, priority 0
}

// Multi-queue aware delivery method
bool RlcReceiverModule::encapsulateAndDeliver(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority) {
    // Check if packet is too small to encapsulate
    if (len == 0) {
        LOG_WARN("[RLC Receiver] Cannot encapsulate empty packet");
        return false;
    }

    // Prefer multi-queue callback if available
    if (multiQueueDeliveryCallback) {
        LOG_DEBUG("[RLC Receiver] Delivering packet to queue " << static_cast<int>(queue_id)
                 << " with priority " << static_cast<int>(priority));
        multiQueueDeliveryCallback(packet, len, queue_id, priority);
        return true;
    }

    // Fallback to regular delivery callback
    if (deliveryCallback) {
        LOG_DEBUG("[RLC Receiver] Delivering packet using legacy callback (queue info lost)");
        deliveryCallback(packet, len);
        return true;
    }

    LOG_WARN("[RLC Receiver] No delivery callback set, cannot deliver packet");
    return false;
}

// Helper function to calculate IP checksum
uint16_t calculateIpChecksum(const void* data, size_t len) {
    const uint16_t* buf = static_cast<const uint16_t*>(data);
    uint32_t sum = 0;
    
    // Sum all 16-bit words
    for (size_t i = 0; i < len / 2; i++) {
        sum += ntohs(buf[i]);
    }
    
    // Handle odd bytes
    if (len & 1) {
        sum += (static_cast<const uint8_t*>(data)[len - 1] << 8);
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return htons(~sum);
}

// Create and send a Status PDU for a completed packet
RlcStatusPdu RlcReceiverModule::createStatusPdu(uint32_t packet_id, bool is_complete) {
    LOG_DEBUG("[RLC Receiver] Starting to create Status PDU for packet ID " << packet_id 
             << ", is_complete=" << (is_complete ? "true" : "false"));

    RlcStatusPdu status_pdu;
    status_pdu.ack_sn = packet_id & RLC_SEQ_NUM_MASK;
    status_pdu.window_size = current_window_size.load(std::memory_order_relaxed);
    status_pdu.timestamp = std::chrono::steady_clock::now();
    
    LOG_DEBUG("[RLC Receiver] Initialized Status PDU with ACK SN=" << packet_id 
             << ", window_size=" << status_pdu.window_size);
    
    if (is_complete) {
        // This is a complete packet, send only ACK
        status_pdu.control = RLC_STATUS_ACK;
        LOG_DEBUG("[RLC Receiver] Setting control flags to ACK (0x" 
                 << std::hex << static_cast<int>(status_pdu.control) << std::dec << ")");
        
        // Remove any pending NACKs for this packet
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = missing_segments.find(packet_id);
            if (it != missing_segments.end()) {
                LOG_DEBUG("[RLC Receiver] Removing " << it->second.size() 
                         << " missing segments for packet ID " << packet_id);
                missing_segments.erase(it);
            } else {
                LOG_DEBUG("[RLC Receiver] No missing segments found for packet ID " << packet_id);
            }
        }
    } else {
        // This is a status report with missing segments (NACKs)
        status_pdu.control = RLC_STATUS_NACK | RLC_STATUS_BITMAP;
        LOG_DEBUG("[RLC Receiver] Setting control flags to NACK+BITMAP (0x" 
                 << std::hex << static_cast<int>(status_pdu.control) << std::dec << ")");
        
        // Collect NACKs for missing segments
        std::vector<RlcNackInfo> nacks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            LOG_DEBUG("[RLC Receiver] Found " << missing_segments.size() 
                     << " packets with missing segments");
            
            for (const auto& entry : missing_segments) {
                uint32_t pid = entry.first;
                
                if (entry.second.empty()) {
                    // Request the entire packet
                    nacks.emplace_back(pid);
                    LOG_DEBUG("[RLC Receiver] Requesting entire packet ID " << pid);
                } else {
                    // Request specific segments
                    LOG_DEBUG("[RLC Receiver] Packet ID " << pid << " has " 
                             << entry.second.size() << " missing segments");
                    
                    for (uint16_t segment_id : entry.second) {
                        nacks.emplace_back(pid, segment_id);
                        LOG_DEBUG("[RLC Receiver] Adding NACK for packet " << pid 
                                 << ", segment " << segment_id);
                    }
                }
                
                // Limit the number of NACKs to prevent oversized PDUs
                if (nacks.size() >= RLC_MAX_NACKS) {
                    LOG_DEBUG("[RLC Receiver] NACK limit reached (" << RLC_MAX_NACKS 
                             << "), truncating NACK list");
                    break;
                }
            }
        }
        
        status_pdu.nack_count = nacks.size();
        status_pdu.nacks = std::move(nacks);
        LOG_DEBUG("[RLC Receiver] Added " << status_pdu.nack_count << " NACKs to Status PDU");
    }
    
    // Increment ACK counter
    uint32_t prev_acks = acksSent.fetch_add(1, std::memory_order_relaxed);
    
    // Calculate serialized PDU size for logging
    std::vector<unsigned char> serialized = status_pdu.serialize();
    
    // Log PDU details
    if (is_complete) {
        LOG_INFO("[RLC Receiver] Created Status PDU: ACK for packet ID " << packet_id 
                << ", total sent ACKs: " << (prev_acks + 1)
                << ", PDU size: " << serialized.size() << " bytes");
    } else {
        LOG_INFO("[RLC Receiver] Created Status PDU: " << status_pdu.nack_count 
                << " NACKs, ACK SN: " << packet_id
                << ", total sent ACKs: " << (prev_acks + 1)
                << ", PDU size: " << serialized.size() << " bytes");
        
        // Log detailed NACK information
        for (size_t i = 0; i < status_pdu.nacks.size() && i < 10; i++) { // Limit to first 10 for readability
            const auto& nack = status_pdu.nacks[i];
            LOG_DEBUG("[RLC Receiver] NACK details [" << i << "]: Packet ID " << nack.packet_id
                     << ", Segment ID " << nack.segment_id
                     << ", Whole packet: " << (nack.segment_id == 0xFFFF ? "true" : "false"));
        }
        
        if (status_pdu.nacks.size() > 10) {
            LOG_DEBUG("[RLC Receiver] ... and " << (status_pdu.nacks.size() - 10) << " more NACKs");
        }
    }
    
    // Log PDU header hex dump for detailed debugging
    if (serialized.size() >= 12) {
        std::stringstream header_hex;
        header_hex << "PDU Header: ";
        for (size_t i = 0; i < 12; i++) {
            header_hex << std::hex << std::setw(2) << std::setfill('0') 
                      << static_cast<int>(serialized[i]) << " ";
        }
        LOG_DEBUG("[RLC Receiver] " << header_hex.str());
    }
    
    return status_pdu;
}

// Internal processing of segment (without immediate status PDU generation)
bool RlcReceiverModule::processSegmentInternal(const RlcSegment& segment) {
    if (!reassemblyManager) {
        LOG_ERROR("[RLC Receiver] No reassembly manager available");
        return false;
    }
    
    uint32_t packet_id = segment.header.original_packet_id & RLC_SEQ_NUM_MASK;
    
    // Track highest sequence number seen for detecting out-of-order packets
    if (packet_id > highest_sn_seen) {
        highest_sn_seen = packet_id;
    }
    
    // Track missing segments for possible NACK generation
    {
        std::lock_guard<std::mutex> lock(mutex);
        
        auto& segments_list = missing_segments[packet_id];
        
        // If this is a new packet or we have an early segment,
        // add all earlier segments to the missing list
        LOG_DEBUG("[RLC Receiver] Checking missing segments for packet ID: " << packet_id << " segment: " 
                  << segment.header.segment_number + 1 << "/" << segment.header.total_segments);
                  
        if (segment.header.segment_number > 0 && 
            (segments_list.empty() || 
             std::find(segments_list.begin(), segments_list.end(), segment.header.segment_number) == segments_list.end())) {
            
            for (uint16_t i = 0; i < segment.header.segment_number; i++) {
                // Check if this segment is already in our missing list
                if (std::find(segments_list.begin(), segments_list.end(), i) == segments_list.end()) {
                    segments_list.push_back(i);
                }
            }
        }
        
        // Remove this segment from missing list if it was there
        segments_list.erase(
            std::remove(segments_list.begin(), segments_list.end(), segment.header.segment_number),
            segments_list.end()
        );
    }
    
    // Try to reassemble with the received segment
    std::vector<unsigned char> reassembled;
    bool packet_complete = reassemblyManager->processSegment(segment, reassembled);
    
    // If a packet is completely reassembled, enqueue it and send a status PDU
    if (packet_complete && !reassembled.empty()) {
        LOG_DEBUG("[RLC Receiver] Reassembled complete packet of size " << reassembled.size() << " bytes");
        packetsReassembled.fetch_add(1, std::memory_order_relaxed);

        if (encapsulateAndDeliver(reassembled.data(), reassembled.size(), segment.header.queue_id, segment.header.priority)) {
            // Only add to pending status PDUs if the packet is complete
            {
                std::lock_guard<std::mutex> lock(status_pdu_mutex);
                pending_status_pdus.insert(packet_id);
                status_pdu_cv.notify_one();
            }

            return true;
        } else {
            packetsDropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[RLC Receiver] Failed to deliver reassembled packet");
            return false;
        }
    }
    
    // Don't add to pending status PDUs for incomplete packets
    // This is the key change - we remove this section so that
    // we don't generate a status PDU for partial packet reception
    
    return true;
}

// Process a segment - This is the main entry point called by higher layers
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
    
    // Check if this is a Status PDU from the sender
    if (segment_data[0] == RLC_PDU_TYPE_STATUS) {
        LOG_DEBUG("[RLC Receiver] Received Status PDU, processing as control message");
        // Process Status PDU from sender (e.g., flow control)
        RlcStatusPdu status_pdu = RlcStatusPdu::deserialize(segment_data, len);
        
        // Handle window size updates or other control information
        if (status_pdu.control & RLC_STATUS_ACK) {
            LOG_DEBUG("[RLC Receiver] Received window update: " << status_pdu.window_size);
            
            // Update our transmission window size based on sender's feedback
            current_window_size.store(status_pdu.window_size, std::memory_order_relaxed);
        }
        
        return true;
    }
    
    // Log detailed information about the received packet
    LOG_DEBUG("[RLC Receiver] Processing packet of size " << len << " bytes");
    
    // Deserialize the segment
    RlcSegment segment = RlcSegment::deserialize(segment_data, len);

    // Check for poll bit
    if (segment.header.poll) {
        LOG_DEBUG("[RLC Receiver] Received segment with poll bit set, ID: " 
                << segment.header.original_packet_id);
        pending_poll.store(true, std::memory_order_relaxed);
        
        // Notify status PDU thread to send a response immediately
        {
            std::lock_guard<std::mutex> lock(status_pdu_mutex);
            status_pdu_cv.notify_one();
        }
    }
    
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
        if (encapsulateAndDeliver(segment.data.data(), segment.data.size(), segment.header.queue_id, segment.header.priority)) {
            packetsReassembled.fetch_add(1, std::memory_order_relaxed);

            // Add to pending status PDUs since this is a complete packet
            {
                std::lock_guard<std::mutex> lock(status_pdu_mutex);
                pending_status_pdus.insert(segment.header.original_packet_id & RLC_SEQ_NUM_MASK);
                status_pdu_cv.notify_one();
            }

            return true;
        } else {
            packetsDropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[RLC Receiver] Failed to deliver non-segmented packet");
            return false;
        }
    }
    
    // For segmented packets, use the internal processing function
    return processSegmentInternal(segment);
}

// Start the status PDU thread
void RlcReceiverModule::startStatusPduThread() {
    if (status_pdu_thread_running.load()) {
        LOG_WARN("[RLC Receiver] Status PDU thread already running");
        return;
    }
    
    status_pdu_thread_running.store(true);
    status_pdu_thread = std::thread(&RlcReceiverModule::statusPduThreadFunc, this);
    
    LOG_INFO("[RLC Receiver] Started Status PDU thread with interval " << status_pdu_interval_ms << "ms");
}

// Stop the status PDU thread
void RlcReceiverModule::stopStatusPduThread() {
    if (!status_pdu_thread_running.load()) {
        return;
    }
    
    status_pdu_thread_running.store(false);
    
    // Notify the thread to wake up if waiting
    status_pdu_cv.notify_one();
    
    // Wait for the thread to join
    if (status_pdu_thread.joinable()) {
        status_pdu_thread.join();
    }
    
    LOG_INFO("[RLC Receiver] Stopped Status PDU thread");
}

// Status PDU thread function
void RlcReceiverModule::statusPduThreadFunc() {
    LOG_DEBUG("[RLC Receiver] Status PDU thread started");
    
    // Periodic cleanup of expired reassembly contexts
    auto last_cleanup_time = std::chrono::steady_clock::now();
    
    // Initialize t_statusProhibit timer
    auto t_statusProhibit_expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(t_statusProhibit_ms);
    bool timer_running = true;
    
    // Create a waiting queue for packets that can't be processed yet
    std::set<uint32_t> waiting_queue;
    
    while (status_pdu_thread_running.load()) {
        // Wait precisely until the timer expires
        {
            std::unique_lock<std::mutex> lock(status_pdu_mutex);
            
            status_pdu_cv.wait_until(lock, t_statusProhibit_expiry, [this]() {
                return !status_pdu_thread_running.load() || 
                    pending_poll.load();
            });
            
            // Check if thread should stop
            if (!status_pdu_thread_running.load()) {
                break;
            }
        }
        
        // Current time after waiting
        auto now = std::chrono::steady_clock::now();

        // Paper §5.4 fast link ACK: poll bit MUST bypass t_statusProhibit timer.
        // Original code only sent ACK when timer expired AND poll_pending — that
        // meant the 35ms timer added latency on every ACK regardless of poll signal,
        // contradicting the paper's "trigger RLC ACKs immediately via poll bits"
        // claim. With this gate-relaxation, ACK delay drops from ~35ms toward ~6ms
        // (paper's reported result, Fig 20).
        bool poll_signal_now = pending_poll.load(std::memory_order_relaxed);
        if (now >= t_statusProhibit_expiry || poll_signal_now) {
            LOG_DEBUG("[RLC Receiver] t_statusProhibit timer expired, processing queued packets");
            
            // Get pending poll bit and packets
            bool poll_pending = pending_poll.exchange(false, std::memory_order_relaxed);
            std::set<uint32_t> new_packets;
            
            {
                std::lock_guard<std::mutex> lock(status_pdu_mutex);
                if (!pending_status_pdus.empty()) {
                    new_packets = std::move(pending_status_pdus);
                    pending_status_pdus.clear();
                }
            }
            
            // Add new packets to waiting queue
            waiting_queue.insert(new_packets.begin(), new_packets.end());
            
            // Then process waiting queue if not empty
            if (!waiting_queue.empty() && statusPduCallback && poll_pending) {
                // Log the status PDU transmission event (once per batch)
                if (status_pdu_logger) {
                    auto current_time = std::chrono::system_clock::now();
                    std::time_t time_now = std::chrono::system_clock::to_time_t(current_time);
                    std::tm tm_now;
                    localtime_r(&time_now, &tm_now);
                    std::ostringstream oss;
                    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") << "," 
                        << acksSent.load() + 1 << "," << waiting_queue.size();
                    status_pdu_logger->logLine(oss.str());
                }
                
                // Increment the acks sent counter (just once for this batch)
                acksSent.fetch_add(1, std::memory_order_relaxed);
                
                for (uint32_t packet_id : waiting_queue) {
                    RlcStatusPdu status_pdu = createStatusPdu(packet_id, true);
                    statusPduCallback(status_pdu);
                    LOG_DEBUG("[RLC Receiver] Status PDU sent for packet ID: " << packet_id);
                }
                
                LOG_INFO("[RLC Receiver] Sent Status PDUs for " << waiting_queue.size() << " packets");
                waiting_queue.clear();
            }
            
            // Reset timer for next interval
            t_statusProhibit_expiry = now + std::chrono::milliseconds(t_statusProhibit_ms) + std::chrono::microseconds(100);
            LOG_DEBUG("[RLC Receiver] Reset t_statusProhibit timer, next expiry in " << t_statusProhibit_ms << "ms");
        } else {
            // Timer hasn't expired yet, just check for new packets to queue
            std::set<uint32_t> new_packets;
            
            {
                std::lock_guard<std::mutex> lock(status_pdu_mutex);
                if (!pending_status_pdus.empty()) {
                    new_packets = std::move(pending_status_pdus);
                    pending_status_pdus.clear();
                }
            }
            
            // Queue new packets if any
            if (!new_packets.empty()) {
                LOG_DEBUG("[RLC Receiver] Timer running, queueing " << new_packets.size() << " packets until next expiry");
                waiting_queue.insert(new_packets.begin(), new_packets.end());
            }
        }
        
        // Cleanup expired reassembly contexts every 500ms
        auto time_since_last_cleanup = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_cleanup_time).count();
            
        if (time_since_last_cleanup >= 500) {
            reassemblyManager->cleanupExpired();
            last_cleanup_time = now;
        }
    }
    
    LOG_DEBUG("[RLC Receiver] Status PDU thread stopped");
}

// Set delivery callback
void RlcReceiverModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
    LOG_INFO("[RLC Receiver] Delivery callback set");
}

// Set multi-queue delivery callback
void RlcReceiverModule::setMultiQueueDeliveryCallback(std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> callback) {
    multiQueueDeliveryCallback = callback;
    LOG_INFO("[RLC Receiver] Multi-queue delivery callback set");
}

// Enqueue a reassembled packet
bool RlcReceiverModule::enqueueReassembledPacket(const unsigned char* packet, size_t len) {
    // Create new RLC packet
    auto rlcPacket = std::make_shared<RlcPacket>(packet, len);
    
    // Try to enqueue to the SPSC queue
    if (!packetBuffer.try_enqueue(rlcPacket)) {
        LOG_WARN("[RLC Receiver] Buffer full, reassembled packet dropped");
        return false;
    }
    
    LOG_DEBUG("[RLC Receiver] Reassembled packet enqueued, size: " << len 
             << " bytes, buffer estimated size: " << getBufferOccupancy());
    return true;
}

// Dequeue a packet
std::shared_ptr<RlcPacket> RlcReceiverModule::dequeuePacket(size_t maxSize) {
    auto* peekedPtr = packetBuffer.peek();
    // Try to peek at the front packet without removing it
    if (peekedPtr == nullptr) {
        return nullptr; // Queue is empty
    }
    
    std::shared_ptr<RlcPacket> packet = *peekedPtr;

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

// Check if there are packets available
bool RlcReceiverModule::hasPackets() const {
    return packetBuffer.size_approx() > 0;
}

// Get buffer occupancy
size_t RlcReceiverModule::getBufferOccupancy() const {
    return packetBuffer.size_approx();
}

// Get buffer capacity
size_t RlcReceiverModule::getBufferCapacity() const {
    return maxBufferSize;
}

// Get statistics
void RlcReceiverModule::getStats(uint32_t& reassembled, uint32_t& dequeued, 
                                 uint32_t& dropped, uint32_t& segments_processed) const {
    reassembled = packetsReassembled.load(std::memory_order_relaxed);
    dequeued = packetsDequeued.load(std::memory_order_relaxed);
    dropped = packetsDropped.load(std::memory_order_relaxed);
    segments_processed = segmentsProcessed.load(std::memory_order_relaxed);
}