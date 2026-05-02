#ifndef RLC_RETRANSMISSION_HPP
#define RLC_RETRANSMISSION_HPP

#include <map>
#include <chrono>
#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>  // For memcpy
#include <functional>
#include <atomic>
#include <arpa/inet.h> // For htonl, ntohl
#include "readerwriterqueue.h"
#include "log.hpp"
#include "rlc_segmentation.hpp" // For RlcSegment definition
#include "async_logger.hpp"

// RLC PDU Types
#define RLC_PDU_TYPE_DATA   0x00
#define RLC_PDU_TYPE_STATUS 0x01

// Control fields for status PDU
#define RLC_STATUS_ACK      0x01 // Acknowledgment
#define RLC_STATUS_NACK     0x02 // Negative Acknowledgment
#define RLC_STATUS_BITMAP   0x04 // NACK with bitmap for selective repeat

// Maximum number of NACKs in a status PDU
#define RLC_MAX_NACKS 32

// Forward declaration for backward compatibility
struct RlcAck {
    uint32_t packet_id;
    uint16_t flags;
};

// Struct for NACK Information
struct RlcNackInfo {
    uint32_t packet_id;  // Original packet ID being NACKed
    uint16_t segment_id; // Specific segment being NACKed (0xFFFF for all segments)
    uint16_t flags;      // Additional flags

    // Default constructor
    RlcNackInfo() : packet_id(0), segment_id(0xFFFF), flags(0) {}
    
    // Constructor with values
    RlcNackInfo(uint32_t pid, uint16_t sid = 0xFFFF, uint16_t f = 0) 
        : packet_id(pid), segment_id(sid), flags(f) {}
};

// RLC Status PDU
struct RlcStatusPdu {
    // Header
    uint8_t  pdu_type;     // PDU type (RLC_PDU_TYPE_STATUS)
    uint8_t  control;      // Control field
    uint16_t reserved;     // Reserved for future use
    uint32_t ack_sn;       // Sequence number being acknowledged
    uint16_t nack_count;   // Number of NACKs in this status PDU
    uint16_t window_size;  // Current receiver window size

    // Optional fields (based on control flags)
    std::chrono::steady_clock::time_point timestamp; // When the status PDU was created// 
    std::chrono::steady_clock::time_point rlc_tx_time; // When the status PDU was sent from RLC
    std::chrono::steady_clock::time_point mac_tx_time; // When the status PDU was sent from MAC
    
    std::vector<RlcNackInfo> nacks;                 // List of NACKs for missing segments

    // Constructors
    RlcStatusPdu()
        : pdu_type(RLC_PDU_TYPE_STATUS),
        control(RLC_STATUS_ACK),
        reserved(0),
        ack_sn(0),
        nack_count(0),
        window_size(0),
        timestamp(std::chrono::steady_clock::now()),
        rlc_tx_time(std::chrono::steady_clock::time_point()),  // Initialize to epoch
        mac_tx_time(std::chrono::steady_clock::time_point())   // Initialize to epoch
    {}

    // Constructor for ACK
    RlcStatusPdu(uint32_t sn, uint16_t window)
        : pdu_type(RLC_PDU_TYPE_STATUS),
        control(RLC_STATUS_ACK),
        reserved(0),
        ack_sn(sn),
        nack_count(0),
        window_size(window),
        timestamp(std::chrono::steady_clock::now()),
        rlc_tx_time(std::chrono::steady_clock::time_point()),  // Initialize to epoch
        mac_tx_time(std::chrono::steady_clock::time_point())   // Initialize to epoch
    {}

    // Constructor for NACK
    RlcStatusPdu(uint32_t sn, uint16_t window, const std::vector<RlcNackInfo>& missing)
        : pdu_type(RLC_PDU_TYPE_STATUS),
        control(RLC_STATUS_NACK),
        reserved(0),
        ack_sn(sn),
        nack_count(missing.size()),
        window_size(window),
        timestamp(std::chrono::steady_clock::now()),
        rlc_tx_time(std::chrono::steady_clock::time_point()),  // Initialize to epoch
        mac_tx_time(std::chrono::steady_clock::time_point()),  // Initialize to epoch
        nacks(missing) {
        
        // Limit the number of NACKs
        if (nacks.size() > RLC_MAX_NACKS) {
            nacks.resize(RLC_MAX_NACKS);
            nack_count = RLC_MAX_NACKS;
        }
        
        // If we have a bitmap, set the appropriate control flag
        if (!nacks.empty()) {
            control |= RLC_STATUS_BITMAP;
        }
    }   
    // Serialize the status PDU to a byte vector
    std::vector<unsigned char> serialize() const {
        // Calculate the size: 12 bytes for header + (8 bytes per NACK)
        size_t size = 12 + (nack_count * 8);
        std::vector<unsigned char> buffer(size, 0);
        
        // Fill in the header
        buffer[0] = pdu_type;
        buffer[1] = control;
        uint16_t net_reserved = htons(reserved);
        memcpy(&buffer[2], &net_reserved, sizeof(net_reserved));
        
        uint32_t net_ack_sn = htonl(ack_sn);
        memcpy(&buffer[4], &net_ack_sn, sizeof(net_ack_sn));
        
        uint16_t net_nack_count = htons(nack_count);
        memcpy(&buffer[8], &net_nack_count, sizeof(net_nack_count));
        
        uint16_t net_window_size = htons(window_size);
        memcpy(&buffer[10], &net_window_size, sizeof(net_window_size));
        
        // Fill in the NACKs
        for (size_t i = 0; i < nack_count && i < nacks.size(); i++) {
            size_t offset = 12 + (i * 8);
            
            uint32_t net_packet_id = htonl(nacks[i].packet_id);
            memcpy(&buffer[offset], &net_packet_id, sizeof(net_packet_id));
            
            uint16_t net_segment_id = htons(nacks[i].segment_id);
            memcpy(&buffer[offset + 4], &net_segment_id, sizeof(net_segment_id));
            
            uint16_t net_flags = htons(nacks[i].flags);
            memcpy(&buffer[offset + 6], &net_flags, sizeof(net_flags));
        }
        
        return buffer;
    }
    
    // Deserialize a byte vector to a status PDU
    static RlcStatusPdu deserialize(const unsigned char* data, size_t len) {
        RlcStatusPdu pdu;
        
        if (len < 12) {
            // Not enough data for header
            return pdu;
        }
        
        // Parse header
        pdu.pdu_type = data[0];
        pdu.control = data[1];
        
        uint16_t net_reserved;
        memcpy(&net_reserved, &data[2], sizeof(net_reserved));
        pdu.reserved = ntohs(net_reserved);
        
        uint32_t net_ack_sn;
        memcpy(&net_ack_sn, &data[4], sizeof(net_ack_sn));
        pdu.ack_sn = ntohl(net_ack_sn);
        
        uint16_t net_nack_count;
        memcpy(&net_nack_count, &data[8], sizeof(net_nack_count));
        pdu.nack_count = ntohs(net_nack_count);
        
        uint16_t net_window_size;
        memcpy(&net_window_size, &data[10], sizeof(net_window_size));
        pdu.window_size = ntohs(net_window_size);
        
        // Parse NACKs if present
        size_t expected_size = 12 + (pdu.nack_count * 8);
        if (len >= expected_size && (pdu.control & RLC_STATUS_NACK)) {
            pdu.nacks.reserve(pdu.nack_count);
            
            for (size_t i = 0; i < pdu.nack_count; i++) {
                size_t offset = 12 + (i * 8);
                
                RlcNackInfo nack;
                
                uint32_t net_packet_id;
                memcpy(&net_packet_id, &data[offset], sizeof(net_packet_id));
                nack.packet_id = ntohl(net_packet_id);
                
                uint16_t net_segment_id;
                memcpy(&net_segment_id, &data[offset + 4], sizeof(net_segment_id));
                nack.segment_id = ntohs(net_segment_id);
                
                uint16_t net_flags;
                memcpy(&net_flags, &data[offset + 6], sizeof(net_flags));
                nack.flags = ntohs(net_flags);
                
                pdu.nacks.push_back(nack);
            }
        }
        
        pdu.timestamp = std::chrono::steady_clock::now();
        return pdu;
    }
};

// Latency tracking structure
struct LatencyMetrics {
    std::chrono::steady_clock::time_point enqueue_time;
    std::chrono::steady_clock::time_point first_dequeue_time;
    std::chrono::steady_clock::time_point last_dequeue_time;
    std::chrono::steady_clock::time_point ack_time;
    
    uint32_t original_size;  // Original packet size in bytes
    uint32_t bytes_acked;    // Bytes that have been acknowledged
    
    bool first_segment_dequeued;
    bool all_segments_dequeued;
    bool is_acked;
    
    LatencyMetrics() 
        : original_size(0),
          bytes_acked(0),
          first_segment_dequeued(false),
          all_segments_dequeued(false),
          is_acked(false) {}
    
    // Calculate end-to-end SDU delay in milliseconds
    int64_t getEndToEndDelay() const {
        if (!is_acked) return -1;
        
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            ack_time - enqueue_time).count();
    }
    
    // Calculate queueing delay in milliseconds (time from enqueue to last segment dequeued)
    int64_t getQueueingDelay() const {
        if (!all_segments_dequeued) return -1;
        
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            last_dequeue_time - enqueue_time).count();
    }
    
    // Calculate ACK delay in milliseconds (time from last segment dequeued to ACK received)
    int64_t getAckDelay() const {
        if (!is_acked || !all_segments_dequeued) return -1;
        
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            ack_time - last_dequeue_time).count();
    }
};

// RLC Retransmission Entry
struct RetransmissionEntry {
    uint32_t packet_id;
    uint32_t original_size;
    std::chrono::steady_clock::time_point enqueue_time;
    uint16_t retransmission_count;
    bool needs_retransmission;
    LatencyMetrics metrics;
    
    // Constructor
    RetransmissionEntry(uint32_t id = 0, uint32_t size = 0)
        : packet_id(id),
          original_size(size),
          enqueue_time(std::chrono::steady_clock::now()),
          retransmission_count(0),
          needs_retransmission(false) {
        
        // Initialize latency metrics
        metrics.enqueue_time = enqueue_time;
        metrics.original_size = size;
    }
};

// RLC Retransmission Manager
class RlcRetransmissionManager {
public:
    RlcRetransmissionManager(uint32_t timeout_ms = 1000, uint16_t max_retries = 5, const std::string& log_dir = "emulator_logs")
        : retransmission_timeout_ms(timeout_ms),
          max_retransmission_attempts(max_retries),
          packets_acked(0),
          bytes_acked(0),
          packets_retransmitted(0),
          packets_dropped(0),
          total_end_to_end_delay_ms(0),
          total_queueing_delay_ms(0),
          total_ack_delay_ms(0),
          total_rlc_to_mac_delay_ms(0),
          total_mac_to_sender_delay_ms(0),
          latest_e2e_delay_ms(0),
          latest_queue_delay_ms(0),
          latest_ack_delay_ms(0),
          latest_rlc_to_mac_delay_ms(0),
          latest_mac_to_sender_delay_ms(0),
          acked_packet_count(0) {
        LOG_INFO("[RLC Retransmission] Manager initialized with timeout " << timeout_ms << "ms, max retries: " << max_retries);
        
        // Create timestamp for log file
        auto now = std::chrono::system_clock::now();
        std::time_t time_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;
        localtime_r(&time_now, &tm_now);
        
        // Create log directory if it doesn't exist
        std::string mkdir_cmd = "mkdir -p " + log_dir;
        int result = system(mkdir_cmd.c_str());
        if (result != 0) {
            LOG_WARN("[RLC Retransmission] Failed to create log directory: " << log_dir);
        }
        
        std::ostringstream oss;
        oss << log_dir << "/rlc_ack_delay_" 
            << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".csv";
        
        // Initialize the async logger for RLC ACK delay
        std::string log_path = oss.str();
        ack_delay_logger = std::make_unique<AsyncLogger>(log_path);
        
        // Write CSV header
        ack_delay_logger->logLine("timestamp,packet_id,ack_delay_ms");
        LOG_INFO("[RLC Retransmission] ACK delay logging to " << log_path);
    }
    
    ~RlcRetransmissionManager() {
        LOG_INFO("[RLC Retransmission] Manager cleaned up. ACKed: " << packets_acked.load()
                << ", Bytes ACKed: " << bytes_acked.load()
                << ", Retransmitted: " << packets_retransmitted.load()
                << ", Dropped: " << packets_dropped.load());
        
        // Log average delays if we have data
        if (acked_packet_count.load() > 0) {
            double avg_e2e = static_cast<double>(total_end_to_end_delay_ms.load()) / acked_packet_count.load();
            double avg_queue = static_cast<double>(total_queueing_delay_ms.load()) / acked_packet_count.load();
            double avg_ack = static_cast<double>(total_ack_delay_ms.load()) / acked_packet_count.load();
            
            LOG_INFO("[RLC Retransmission] Average delays (ms) - End-to-end: " << avg_e2e 
                   << ", Queueing: " << avg_queue
                   << ", ACK: " << avg_ack);
        }
    }
    
    // Add a packet to the retransmission buffer
    void addPacket(uint32_t packet_id, uint32_t original_size) {
        std::lock_guard<std::mutex> lock(retransmission_mutex);

        // Mask the packet ID to 12 bits when storing
        uint32_t masked_id = packet_id & RLC_SEQ_NUM_MASK;
        
        RetransmissionEntry entry(packet_id, original_size);
        retransmission_buffer[packet_id] = entry;
        
        LOG_DEBUG("[RLC Retransmission] Added packet ID " << packet_id 
                 << " of size " << original_size << " to retransmission buffer");
    }
    
    // Record when a segment has been dequeued for transmission
    void recordSegmentDequeued(uint32_t packet_id, uint16_t segment_number, uint16_t total_segments, size_t segment_size) {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        
        auto it = retransmission_buffer.find(packet_id);
        if (it == retransmission_buffer.end()) {
            LOG_WARN("[RLC Retransmission] Tried to record dequeue for unknown packet ID: " << packet_id);
            return;
        }
        
        auto& entry = it->second;
        auto& metrics = entry.metrics;
        
        // If this is the first segment, record first dequeue time
        if (!metrics.first_segment_dequeued) {
            metrics.first_segment_dequeued = true;
            metrics.first_dequeue_time = std::chrono::steady_clock::now();
        }
        
        // Always update last dequeue time
        metrics.last_dequeue_time = std::chrono::steady_clock::now();
        
        // If this is the last segment, mark all segments as dequeued
        if (segment_number == total_segments - 1) {
            metrics.all_segments_dequeued = true;
            int64_t queue_delay = metrics.getQueueingDelay();
            if (queue_delay > 0) {
                latest_queue_delay_ms.store(queue_delay, std::memory_order_relaxed);
            }
            LOG_DEBUG("[RLC Retransmission] All segments of packet ID " << packet_id 
                     << " have been dequeued, queueing delay: " << queue_delay << "ms");
        }
    }
    
    // Process a Status PDU
    bool processStatusPdu(const RlcStatusPdu& status_pdu) {
        std::lock_guard<std::mutex> lock(retransmission_mutex);

        last_status_pdu_time = std::chrono::steady_clock::now();
        
        // Process ACK
        if (status_pdu.control & RLC_STATUS_ACK) {
            uint32_t packet_id = status_pdu.ack_sn;
            auto it = retransmission_buffer.find(packet_id);
            
            if (it == retransmission_buffer.end()) {
                LOG_WARN("[RLC Retransmission] Received ACK for unknown packet ID: " << packet_id);
                return false;
            }
            
            auto& entry = it->second;
            auto& metrics = entry.metrics;
            
            // Record ACK time and calculate delays
            metrics.ack_time = status_pdu.timestamp;
            metrics.is_acked = true;
            metrics.bytes_acked = entry.original_size; // Mark all bytes as ACKed
            
            int64_t e2e_delay = metrics.getEndToEndDelay();
            int64_t queue_delay = metrics.getQueueingDelay();
            int64_t ack_delay = metrics.getAckDelay();
            
            // Store the latest delay values
            latest_e2e_delay_ms.store(e2e_delay, std::memory_order_relaxed);
            latest_queue_delay_ms.store(queue_delay, std::memory_order_relaxed);
            latest_ack_delay_ms.store(ack_delay, std::memory_order_relaxed);
            
            LOG_DEBUG("[RLC Retransmission] ACK received for packet ID " << packet_id 
                     << ". Delays (ms) - E2E: " << e2e_delay 
                     << ", Queue: " << queue_delay
                     << ", ACK: " << ack_delay);
                     
            // Log the RLC ACK delay to the CSV file
            if (ack_delay_logger && ack_delay > 0) {
                auto now = std::chrono::system_clock::now();
                std::time_t time_now = std::chrono::system_clock::to_time_t(now);
                std::tm tm_now;
                localtime_r(&time_now, &tm_now);
                std::ostringstream oss;
                oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") << "," << packet_id << "," << ack_delay;
                ack_delay_logger->logLine(oss.str());
            }
            
            // Update statistics
            packets_acked.fetch_add(1, std::memory_order_relaxed);
            bytes_acked.fetch_add(entry.original_size, std::memory_order_relaxed);
            
            if (e2e_delay > 0) {
                total_end_to_end_delay_ms.fetch_add(e2e_delay, std::memory_order_relaxed);
                acked_packet_count.fetch_add(1, std::memory_order_relaxed);
            }
            
            if (queue_delay > 0) {
                total_queueing_delay_ms.fetch_add(queue_delay, std::memory_order_relaxed);
            }
            
            if (ack_delay > 0) {
                total_ack_delay_ms.fetch_add(ack_delay, std::memory_order_relaxed);
            }
            
            // Calculate RLC to MAC and MAC to sender delays
            int64_t rlc_to_mac_delay = 0;
            int64_t mac_to_sender_delay = 0;
             
            if (status_pdu.rlc_tx_time.time_since_epoch().count() > 0 && 
                status_pdu.mac_tx_time.time_since_epoch().count() > 0) {
                
                // Log the raw timestamp values for debugging
                int64_t timestamp_raw = status_pdu.timestamp.time_since_epoch().count();
                int64_t rlc_tx_raw = status_pdu.rlc_tx_time.time_since_epoch().count();
                int64_t mac_tx_raw = status_pdu.mac_tx_time.time_since_epoch().count();
                int64_t ack_time_raw = metrics.ack_time.time_since_epoch().count();
                
                LOG_DEBUG("[RLC Retransmission] Timestamp values (ns) for packet ID " << packet_id 
                        << " - Creation: " << timestamp_raw 
                        << ", RLC TX: " << rlc_tx_raw
                        << ", MAC TX: " << mac_tx_raw
                        << ", ACK time: " << ack_time_raw);
                
                rlc_to_mac_delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                    status_pdu.mac_tx_time - status_pdu.rlc_tx_time).count();
                    
                mac_to_sender_delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                    metrics.ack_time - status_pdu.mac_tx_time).count();
                
                // Log the calculated delays
                LOG_DEBUG("[RLC Retransmission] Calculated delays (ms) for packet ID " << packet_id 
                        << " - RLC to MAC: " << rlc_to_mac_delay
                        << ", MAC to sender: " << mac_to_sender_delay);
                
                // Only update if values are reasonable (e.g., non-negative and not absurdly large)
                if (rlc_to_mac_delay >= 0 && rlc_to_mac_delay < 3600000 && 
                    mac_to_sender_delay >= 0 && mac_to_sender_delay < 3600000) {
                    
                    // Update total and latest values
                    total_rlc_to_mac_delay_ms.fetch_add(rlc_to_mac_delay, std::memory_order_relaxed);
                    total_mac_to_sender_delay_ms.fetch_add(mac_to_sender_delay, std::memory_order_relaxed);
                    latest_rlc_to_mac_delay_ms.store(rlc_to_mac_delay, std::memory_order_relaxed);
                    latest_mac_to_sender_delay_ms.store(mac_to_sender_delay, std::memory_order_relaxed);
                } else {
                    LOG_WARN("[RLC Retransmission] Unreasonable delay values detected for packet ID " 
                            << packet_id << ": RLC to MAC = " << rlc_to_mac_delay 
                            << "ms, MAC to sender = " << mac_to_sender_delay << "ms");
                }
            }


            // Remove from retransmission buffer
            retransmission_buffer.erase(it);
            return true;
        }
        
        // Process NACKs
        if (status_pdu.control & RLC_STATUS_NACK) {
            for (const auto& nack : status_pdu.nacks) {
                uint32_t packet_id = nack.packet_id;
                auto it = retransmission_buffer.find(packet_id);
                
                if (it != retransmission_buffer.end()) {
                    // Mark for retransmission
                    it->second.needs_retransmission = true;
                    it->second.retransmission_count++;
                }
            }
        }
        
        return true;
    }
    
    // For backward compatibility with existing code
    bool processAck(const RlcAck& ack) {
        // Create a Status PDU from the ACK
        RlcStatusPdu status_pdu(ack.packet_id, 0);
        return processStatusPdu(status_pdu);
    }
    
    // Check for expired packets that need retransmission
    std::vector<uint32_t> checkForRetransmissions() {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> packets_to_retransmit;
        
        for (auto it = retransmission_buffer.begin(); it != retransmission_buffer.end();) {
            auto& entry = it->second;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry.enqueue_time).count();
                
            if (elapsed > retransmission_timeout_ms) {
                if (entry.retransmission_count >= max_retransmission_attempts) {
                    // Max retries reached, drop the packet
                    LOG_WARN("[RLC Retransmission] Packet ID " << entry.packet_id 
                            << " exceeded max retries (" << max_retransmission_attempts 
                            << "), dropping");
                    packets_dropped.fetch_add(1, std::memory_order_relaxed);
                    it = retransmission_buffer.erase(it);
                    continue;
                }
                
                // Mark for retransmission
                entry.needs_retransmission = true;
                entry.retransmission_count++;
                entry.enqueue_time = now; // Reset the timer
                
                packets_to_retransmit.push_back(entry.packet_id);
                packets_retransmitted.fetch_add(1, std::memory_order_relaxed);
                
                LOG_DEBUG("[RLC Retransmission] Packet ID " << entry.packet_id 
                         << " marked for retransmission, attempt " << entry.retransmission_count);
            }
            
            ++it;
        }
        
        return packets_to_retransmit;
    }
    
    // Check if a specific packet needs retransmission
    bool needsRetransmission(uint32_t packet_id) {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        
        auto it = retransmission_buffer.find(packet_id);
        if (it == retransmission_buffer.end()) {
            return false;
        }
        
        return it->second.needs_retransmission;
    }
    
    // Mark a packet as retransmitted
    void markRetransmitted(uint32_t packet_id) {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        
        auto it = retransmission_buffer.find(packet_id);
        if (it == retransmission_buffer.end()) {
            return;
        }
        
        it->second.needs_retransmission = false;
    }
    
    // Add this new method or update the existing one
    void getStats(uint32_t& acked, uint32_t& bytes_ack, uint32_t& retransmitted, 
                uint32_t& dropped, double& avg_e2e_delay, double& avg_queue_delay, 
                double& avg_ack_delay, double& avg_rlc_to_mac_delay, double& avg_mac_to_sender_delay,
                int64_t& latest_e2e, int64_t& latest_queue, int64_t& latest_ack,
                int64_t& latest_rlc_to_mac, int64_t& latest_mac_to_sender) {

        // Check and reset values if needed
        //checkAndResetDelayValues();
        
        acked = packets_acked.load(std::memory_order_relaxed);
        bytes_ack = bytes_acked.load(std::memory_order_relaxed);
        retransmitted = packets_retransmitted.load(std::memory_order_relaxed);
        dropped = packets_dropped.load(std::memory_order_relaxed);
        
        uint32_t count = acked_packet_count.load(std::memory_order_relaxed);
        if (count > 0) {
            avg_e2e_delay = static_cast<double>(total_end_to_end_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_queue_delay = static_cast<double>(total_queueing_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_ack_delay = static_cast<double>(total_ack_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_rlc_to_mac_delay = static_cast<double>(total_rlc_to_mac_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_mac_to_sender_delay = static_cast<double>(total_mac_to_sender_delay_ms.load(std::memory_order_relaxed)) / count;
        } else {
            avg_e2e_delay = 0.0;
            avg_queue_delay = 0.0;
            avg_ack_delay = 0.0;
            avg_rlc_to_mac_delay = 0.0;
            avg_mac_to_sender_delay = 0.0;
        }
        
        // Return the latest delay values
        latest_e2e = latest_e2e_delay_ms.load(std::memory_order_relaxed);
        latest_queue = latest_queue_delay_ms.load(std::memory_order_relaxed);
        latest_ack = latest_ack_delay_ms.load(std::memory_order_relaxed);
        latest_rlc_to_mac = latest_rlc_to_mac_delay_ms.load(std::memory_order_relaxed);
        latest_mac_to_sender = latest_mac_to_sender_delay_ms.load(std::memory_order_relaxed);


        // Reset bytes_acked to 0 after retrieving the value
        //bytes_acked.store(0, std::memory_order_relaxed);
    }

    // Get statistics with detailed latency metrics including latest values
    void getStats(uint32_t& acked, uint32_t& bytes_ack, uint32_t& retransmitted, 
                 uint32_t& dropped, double& avg_e2e_delay, double& avg_queue_delay, 
                 double& avg_ack_delay, int64_t& latest_e2e, int64_t& latest_queue, 
                 int64_t& latest_ack) {
        acked = packets_acked.load(std::memory_order_relaxed);
        bytes_ack = bytes_acked.load(std::memory_order_relaxed);
        retransmitted = packets_retransmitted.load(std::memory_order_relaxed);
        dropped = packets_dropped.load(std::memory_order_relaxed);
        
        uint32_t count = acked_packet_count.load(std::memory_order_relaxed);
        if (count > 0) {
            avg_e2e_delay = static_cast<double>(total_end_to_end_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_queue_delay = static_cast<double>(total_queueing_delay_ms.load(std::memory_order_relaxed)) / count;
            avg_ack_delay = static_cast<double>(total_ack_delay_ms.load(std::memory_order_relaxed)) / count;
        } else {
            avg_e2e_delay = 0.0;
            avg_queue_delay = 0.0;
            avg_ack_delay = 0.0;
        }
        
        // Return the latest delay values
        latest_e2e = latest_e2e_delay_ms.load(std::memory_order_relaxed);
        latest_queue = latest_queue_delay_ms.load(std::memory_order_relaxed);
        latest_ack = latest_ack_delay_ms.load(std::memory_order_relaxed);

        // Reset bytes_acked to 0 after retrieving the value
        //bytes_acked.store(0, std::memory_order_relaxed);
    }
    
    // Get statistics with detailed latency metrics (previous version for compatibility)
    void getStats(uint32_t& acked, uint32_t& bytes_ack, uint32_t& retransmitted, 
                 uint32_t& dropped, double& e2e_delay, double& queue_delay, 
                 double& ack_delay) {
        int64_t latest_e2e, latest_queue, latest_ack;
        getStats(acked, bytes_ack, retransmitted, dropped, e2e_delay, queue_delay, 
                ack_delay, latest_e2e, latest_queue, latest_ack);
    }
    
    // Get basic statistics (for backward compatibility)
    void getStats(uint32_t& acked, uint32_t& retransmitted, uint32_t& dropped) {
        acked = packets_acked.load(std::memory_order_relaxed);
        retransmitted = packets_retransmitted.load(std::memory_order_relaxed);
        dropped = packets_dropped.load(std::memory_order_relaxed);
        
        // Reset bytes_acked to 0 after retrieving the value
        //bytes_acked.store(0, std::memory_order_relaxed);
    }
    
    // Get number of pending packets
    size_t getPendingPacketsCount() const {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        return retransmission_buffer.size();
    }
    
    // Get latency metrics for a specific packet
    LatencyMetrics getPacketMetrics(uint32_t packet_id) const {
        std::lock_guard<std::mutex> lock(retransmission_mutex);
        
        auto it = retransmission_buffer.find(packet_id);
        if (it != retransmission_buffer.end()) {
            return it->second.metrics;
        }
        
        return LatencyMetrics(); // Return default metrics if not found
    }
    
private:
    std::map<uint32_t, RetransmissionEntry> retransmission_buffer;
    mutable std::mutex retransmission_mutex;
    uint32_t retransmission_timeout_ms;
    uint16_t max_retransmission_attempts;
    
    std::atomic<uint32_t> packets_acked;
    std::atomic<uint32_t> bytes_acked;
    std::atomic<uint32_t> packets_retransmitted;
    std::atomic<uint32_t> packets_dropped;
    
    // Delay tracking - average values
    std::atomic<int64_t> total_end_to_end_delay_ms;
    std::atomic<int64_t> total_queueing_delay_ms;
    std::atomic<int64_t> total_ack_delay_ms;
    std::atomic<int64_t> total_rlc_to_mac_delay_ms;
    std::atomic<int64_t> total_mac_to_sender_delay_ms;
    
    // Delay tracking - latest values
    std::atomic<int64_t> latest_e2e_delay_ms;
    std::atomic<int64_t> latest_queue_delay_ms;
    std::atomic<int64_t> latest_ack_delay_ms;
    std::atomic<int64_t> latest_rlc_to_mac_delay_ms;
    std::atomic<int64_t> latest_mac_to_sender_delay_ms;
    
    std::atomic<uint32_t> acked_packet_count;
    
    // Logger for RLC ACK delay
    std::unique_ptr<AsyncLogger> ack_delay_logger;


    // Add this line to track the last time a status PDU was processed
    std::chrono::steady_clock::time_point last_status_pdu_time;
    
    // Add this method to check and reset values if needed
    void checkAndResetDelayValues() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_status_pdu_time).count();
            
        // If no updates for 500ms, reset all delay values
        if (elapsed > 500) {
            latest_e2e_delay_ms.store(0, std::memory_order_relaxed);
            latest_queue_delay_ms.store(0, std::memory_order_relaxed);
            latest_ack_delay_ms.store(0, std::memory_order_relaxed);
            latest_rlc_to_mac_delay_ms.store(0, std::memory_order_relaxed);
            latest_mac_to_sender_delay_ms.store(0, std::memory_order_relaxed);
            
            LOG_DEBUG("[RLC Retransmission] Reset all delay values due to inactivity for " 
                     << elapsed << "ms");
        }
    }
};

#endif // RLC_RETRANSMISSION_HPP