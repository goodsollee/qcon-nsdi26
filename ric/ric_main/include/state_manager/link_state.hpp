#ifndef LINK_STATE_HPP
#define LINK_STATE_HPP

#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <memory>
#include <chrono>
#include <jsoncpp/json/json.h>

namespace ric {

/**
 * LinkState - Class to store and manage the state of a network link
 * Optimized for DU metrics and efficient access patterns
 */
class LinkState {
public:
    // Constructor with link ID
    LinkState(int link_id);
    
    // Get link identifier
    const int getLinkId() const { return link_id_; }
    
    // Update metrics from DU stats
    void updateFromRlcMetrics(const Json::Value& metrics);
    void updatePdcpMetrics(int64_t bytes);
    
    // Get current performance metrics
    double getThroughputMbps() const; 
    double getBufferOccupancyBytes() const;
    double getAverageDelayMs() const;
    double getPacketLossRate() const;
    
    // Get historical metrics
    std::vector<double> getThroughputHistory(size_t window_size = 0) const;
    std::vector<double> getDelayHistory(size_t window_size = 0) const;
    
    // Get time since last update
    std::chrono::milliseconds getTimeSinceLastUpdate() const;
    
    // Check if link is active based on recent updates
    bool isActive(std::chrono::milliseconds max_inactive_time = std::chrono::seconds(5)) const;
    
    // Serialize current state to JSON
    Json::Value toJson() const;
    
private:
    // Link identifier
    int link_id_;
    
    int64_t pdcp_transmitted_bytes_ = 0; // Total PDCP transmitted bytes

    // Current metrics
    double throughput_mbps_ = 0.0;           // Current throughput in Mbps
    double buffer_occupancy_bytes_ = 0.0;    // Current buffer occupancy in bytes
    double packet_loss_rate_ = 0.0;          // Current packet loss rate (0-1)
    uint64_t rlc_acked_bytes_ = 0;           // Total RLC acknowledged bytes

    double avg_e2e_delay_ms_ = 0; // Average end-to-end delay in ms
    double avg_ack_delay_ms_ = 0; // Average ACK delay in ms
    double avg_queue_delay_ms_ = 0; // Average queue delay in ms

    double latest_e2e_delay_ms_ = 0; // Latest end-to-end delay in ms
    double latest_ack_delay_ms_ = 0; // Latest ACK delay in ms
    double latest_queue_delay_ms_ = 0; // Latest queue delay in ms
    
    // History data (using circular buffers)
    static const size_t DEFAULT_HISTORY_SIZE = 20;
    std::deque<double> throughput_history_;
    std::deque<double> delay_history_;
    std::deque<double> buffer_history_;
    std::deque<double> packet_loss_history_;
    
    // Timestamp of last update
    std::chrono::steady_clock::time_point last_update_time_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Helper methods
    void updateThroughput(const Json::Value& metrics);
    void updateBufferOccupancy(const Json::Value& metrics);
    void updateDelayMetrics(const Json::Value& metrics);
    void updatePacketLossMetrics(const Json::Value& metrics);
    
    // Calculate moving average from a history deque
    double calculateMovingAverage(const std::deque<double>& history, size_t window_size = 0) const;
};

} // namespace ric

#endif // LINK_STATE_HPP