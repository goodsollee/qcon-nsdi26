#include "state_manager/link_state.hpp"
#include "log.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

const std::string MODULE = "LinkState";

namespace ric {

LinkState::LinkState(int link_id)
    : link_id_(link_id),
      last_update_time_(std::chrono::steady_clock::now()) {
}

void LinkState::updateFromRlcMetrics(const Json::Value& metrics) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update last update time
    last_update_time_ = std::chrono::steady_clock::now();
    
    // Update individual metrics
    updateThroughput(metrics);
    updateBufferOccupancy(metrics);
    updateDelayMetrics(metrics);
    updatePacketLossMetrics(metrics);
    
    LOG_MODULE_DEBUG(MODULE, "Updated link state for " << link_id_ << ": throughput=" << throughput_mbps_ 
              << "Mbps, delay=" << avg_e2e_delay_ms_ << "ms");
}

void LinkState::updatePdcpMetrics(int64_t bytes) {
    pdcp_transmitted_bytes_ = bytes;
}

void LinkState::updateThroughput(const Json::Value& metrics) {
    if (metrics.isMember("rlc_acked_bytes")) {
        // Store the RLC acknowledged bytes for tracking
        rlc_acked_bytes_ = metrics["rlc_acked_bytes"].asUInt64();
        
        // Calculate throughput in Mbps
        // Note: Convert bytes to bits (x8) and then to Mbps (/1000000)
        // Assuming timestamps are in milliseconds
        // So throughput = bytes * 8 / time_diff_ms * 1000 / 1000000
        /*
        static uint64_t prev_acked_bytes = 0;
        static uint64_t prev_timestamp = 0;
        
        uint64_t acked_bytes = metrics["rlc_acked_bytes"].asUInt64();
        uint64_t timestamp = metrics["timestamp"].asUInt64();
        
        if (prev_timestamp > 0) {
            uint64_t bytes_diff = acked_bytes - prev_acked_bytes;
            uint64_t time_diff_ms = timestamp - prev_timestamp;
            
            if (time_diff_ms > 0) {
                throughput_mbps_ = (bytes_diff * 8.0) / (time_diff_ms / 1000.0) / 1000000.0;
                
                // Add to history
                throughput_history_.push_back(throughput_mbps_);
                while (throughput_history_.size() > DEFAULT_HISTORY_SIZE) {
                    throughput_history_.pop_front();
                }
            }
        }
        
        prev_acked_bytes = acked_bytes;
        prev_timestamp = timestamp;
        */
        throughput_mbps_ = metrics["throughput_mbps"].asDouble();
    }
}

void LinkState::updateBufferOccupancy(const Json::Value& metrics) {
    if (metrics.isMember("rlc_total_buffer_bytes")) {
        buffer_occupancy_bytes_ = metrics["rlc_total_buffer_bytes"].asDouble();
        
        // Add to history
        buffer_history_.push_back(buffer_occupancy_bytes_);
        while (buffer_history_.size() > DEFAULT_HISTORY_SIZE) {
            buffer_history_.pop_front();
        }
    }
}

void LinkState::updateDelayMetrics(const Json::Value& metrics) {
    // Handle average delay metrics
    if (metrics.isMember("rlc_avg_e2e_delay_ms")) {
        avg_e2e_delay_ms_ = metrics["rlc_avg_e2e_delay_ms"].asDouble();
    }
    
    if (metrics.isMember("rlc_avg_queue_delay_ms")) {
        avg_queue_delay_ms_ = metrics["rlc_avg_queue_delay_ms"].asDouble();
    }
    
    if (metrics.isMember("rlc_avg_ack_delay_ms")) {
        avg_ack_delay_ms_ = metrics["rlc_avg_ack_delay_ms"].asDouble();
    }

    // Handle latest delay metrics
    if (metrics.isMember("rlc_latest_e2e_delay_ms")) {
        latest_e2e_delay_ms_ = metrics["rlc_latest_e2e_delay_ms"].asDouble();
    }
    
    if (metrics.isMember("rlc_latest_queue_delay_ms")) {
        latest_queue_delay_ms_ = metrics["rlc_latest_queue_delay_ms"].asDouble();
    }
    
    if (metrics.isMember("rlc_latest_ack_delay_ms")) {
        latest_ack_delay_ms_ = metrics["rlc_latest_ack_delay_ms"].asDouble();
    }
}


void LinkState::updatePacketLossMetrics(const Json::Value& metrics) {
    if (metrics.isMember("rlc_acked_packets") && metrics.isMember("rlc_dropped_packets")) {
        uint64_t acked_packets = metrics["rlc_acked_packets"].asUInt64();
        uint64_t dropped_packets = metrics["rlc_dropped_packets"].asUInt64();
        
        if (acked_packets + dropped_packets > 0) {
            packet_loss_rate_ = static_cast<double>(dropped_packets) / (acked_packets + dropped_packets);
            
            // Add to history
            packet_loss_history_.push_back(packet_loss_rate_);
            while (packet_loss_history_.size() > DEFAULT_HISTORY_SIZE) {
                packet_loss_history_.pop_front();
            }
        }
    }
}

double LinkState::getThroughputMbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return throughput_mbps_;
}

double LinkState::getBufferOccupancyBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_occupancy_bytes_;
}

double LinkState::getAverageDelayMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return avg_e2e_delay_ms_;
}

double LinkState::getPacketLossRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packet_loss_rate_;
}

std::vector<double> LinkState::getThroughputHistory(size_t window_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (window_size == 0 || window_size > throughput_history_.size()) {
        return std::vector<double>(throughput_history_.begin(), throughput_history_.end());
    } else {
        auto start = throughput_history_.end() - window_size;
        return std::vector<double>(start, throughput_history_.end());
    }
}

std::vector<double> LinkState::getDelayHistory(size_t window_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (window_size == 0 || window_size > delay_history_.size()) {
        return std::vector<double>(delay_history_.begin(), delay_history_.end());
    } else {
        auto start = delay_history_.end() - window_size;
        return std::vector<double>(start, delay_history_.end());
    }
}

std::chrono::milliseconds LinkState::getTimeSinceLastUpdate() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time_);
}

bool LinkState::isActive(std::chrono::milliseconds max_inactive_time) const {
    return getTimeSinceLastUpdate() < max_inactive_time;
}

Json::Value LinkState::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Json::Value root;
    root["link_id"] = link_id_;
    root["throughput_mbps"] = throughput_mbps_;
    root["buffer_occupancy_bytes"] = buffer_occupancy_bytes_;
    root["avg_e2e_delay_ms"] = avg_e2e_delay_ms_;
    
    // PDCP
    root["pdcp_transmitted_bytes"] = Json::Value::UInt64(pdcp_transmitted_bytes_);

    // Add the new fields
    root["latest_e2e_delay_ms"] = latest_e2e_delay_ms_; 
    root["avg_queue_delay_ms"] = avg_queue_delay_ms_;
    root["avg_ack_delay_ms"] = avg_ack_delay_ms_;
    root["latest_queue_delay_ms"] = latest_queue_delay_ms_;
    root["latest_ack_delay_ms"] = latest_ack_delay_ms_;
    
    // Add RLC acked bytes
    root["rlc_acked_bytes"] = Json::Value::UInt64(rlc_acked_bytes_);
    
    root["packet_loss_rate"] = packet_loss_rate_;
    
    auto time_since_update = getTimeSinceLastUpdate();
    root["time_since_update_ms"] = static_cast<uint64_t>(time_since_update.count());
    root["active"] = isActive();
    
    return root;
}

double LinkState::calculateMovingAverage(const std::deque<double>& history, size_t window_size) const {
    if (history.empty()) {
        return 0.0;
    }
    
    if (window_size == 0 || window_size > history.size()) {
        window_size = history.size();
    }
    
    auto start = history.end() - window_size;
    return std::accumulate(start, history.end(), 0.0) / window_size;
}

} // namespace ric