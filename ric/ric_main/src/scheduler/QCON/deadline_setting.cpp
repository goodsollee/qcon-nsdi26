#include "scheduler/QCON/deadline_setting.hpp"
#include "log.hpp"
#include <algorithm>
#include <cmath>

namespace ric {

const std::string MODULE = "DEADLINE_SETTING";

DeadlineSettings::DeadlineSettings() {
    LOG_MODULE_DEBUG(MODULE, "DeadlineSettings created");
}

double DeadlineSettings::calculateExpectedDeliveryTime(
    const std::map<int, double>& frame_bytes,
    const std::map<int, double>& link_bandwidths,
    const std::map<int, double>& link_latencies,
    const std::map<int, double>& link_buffer_bytes) {
    
        // Vector to store completion times for each link
    std::vector<double> link_completion_times;
    
    // Calculate completion time for each link based on its assigned bytes
    for (const auto& pair : frame_bytes) {
        int link_id = pair.first;
        double chunk_size_kb = pair.second;
        
        // Skip links with zero chunk size
        if (chunk_size_kb <= 0) {
            continue;
        }
        
        // Get link attributes or default values if not found
        double bandwidth_mbps = 1.0;  // Default 1 Mbps
        double latency_ms = 50.0;     // Default 50ms
        double buffer_bytes = 0.0;    // Default empty buffer
        
        auto bw_it = link_bandwidths.find(link_id);
        if (bw_it != link_bandwidths.end()) {
            bandwidth_mbps = bw_it->second;
        }
        
        auto lat_it = link_latencies.find(link_id);
        if (lat_it != link_latencies.end()) {
            latency_ms = lat_it->second;
        }
        
        auto buf_it = link_buffer_bytes.find(link_id);
        if (buf_it != link_buffer_bytes.end()) {
            buffer_bytes = buf_it->second;
        }
        
        // Calculate transmission time (KB to bits, then divide by Mbps to get ms)
        double tx_time_ms = (chunk_size_kb * 8.0) / bandwidth_mbps;
        
        // Calculate buffer delay
        double buffer_delay_ms = 0.0;
        if (bandwidth_mbps > 0) {
            // Simple model: buffer delay = buffer size in bits / bandwidth in bits per ms
            buffer_delay_ms = (buffer_bytes * 8.0) / (bandwidth_mbps * 1000.0);
        }
        
        // Total delivery time for this chunk
        double chunk_time_ms = tx_time_ms + latency_ms + buffer_delay_ms;
        
        link_completion_times.push_back(chunk_time_ms);
        
        LOG_MODULE_DEBUG(MODULE, "Link " << link_id << 
                      ", chunk=" << chunk_size_kb << "KB, tx_time=" << tx_time_ms << 
                      "ms, latency=" << latency_ms << "ms, buffer_delay=" << 
                      buffer_delay_ms << "ms, total=" << chunk_time_ms << "ms");
    }
    
    // The expected delivery time is the maximum completion time across all links
    double max_completion_time = 0.0;
    if (!link_completion_times.empty()) {
        max_completion_time = *std::max_element(
            link_completion_times.begin(), link_completion_times.end());
    }
    
    LOG_MODULE_DEBUG(MODULE, "Expected delivery time: " << max_completion_time << "ms");
    
    return max_completion_time;
}

double DeadlineSettings::calculateOptimalDeadline(
    double last_frame_completion_time_ms,
    double avg_frame_size_kb, 
    double target_fps) {
    
    // Base deadline calculation: 1000ms / target FPS
    double base_deadline_ms = 1000.0 / target_fps;
    
    // Apply jitter buffer factor
    double jitter_buffer_ms = base_deadline_ms * JITTER_BUFFER_FRAMES;
    
    // Use last frame completion time as a baseline
    double adaptive_deadline_ms = last_frame_completion_time_ms * 1.1;  // 10% margin
    
    // Take the maximum of the base deadline and adaptive deadline
    double optimal_deadline_ms = std::max(base_deadline_ms + jitter_buffer_ms, adaptive_deadline_ms);
    
    // Clamp to min/max bounds
    optimal_deadline_ms = std::max(MIN_DEADLINE_MS, std::min(MAX_DEADLINE_MS, optimal_deadline_ms));
    
    LOG_MODULE_DEBUG(MODULE, "Calculated optimal deadline: " << optimal_deadline_ms << 
                   "ms (base: " << base_deadline_ms << "ms, adaptive: " << 
                   adaptive_deadline_ms << "ms)");
    
    return optimal_deadline_ms;
}

bool DeadlineSettings::shouldUseMultilink(
    double expected_delivery_time_ms,
    double deadline_ms,
    double deadline_threshold) {
    
    // Calculate the ratio of expected delivery time to deadline
    double time_ratio = expected_delivery_time_ms / deadline_ms;
    
    // If expected time exceeds threshold ratio of deadline, use multilink
    bool use_multilink = (time_ratio > deadline_threshold);
    
    LOG_MODULE_DEBUG(MODULE, "Multilink decision: " << (use_multilink ? "YES" : "NO") << 
                   ", time ratio: " << time_ratio << 
                   ", threshold: " << deadline_threshold);
    
    return use_multilink;
}

double DeadlineSettings::calculateBufferDelay(double buffer_bytes, double bandwidth_mbps) {
    // Convert bandwidth from Mbps to bytes per ms
    double bytes_per_ms = (bandwidth_mbps * 1024 * 1024) / (8.0 * 1000.0);
    
    // Calculate delay based on buffer occupancy and bandwidth
    double buffer_delay_ms = 0.0;
    
    if (bytes_per_ms > 0) {
        buffer_delay_ms = buffer_bytes / bytes_per_ms;
    }
    
    return buffer_delay_ms;
}

} // namespace ric