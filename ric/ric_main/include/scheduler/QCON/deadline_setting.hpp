#ifndef DEADLINE_SETTING_HPP
#define DEADLINE_SETTING_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace ric {

/**
 * DeadlineSettings - Class for calculating and managing frame delivery deadlines
 * 
 * This class calculates expected delivery times based on:
 * - Last frame completion time
 * - Remaining bytes to be transmitted
 * - Current bandwidths of all available links
 * - Scheduling decisions
 */
class DeadlineSettings {
public:
    /**
     * Constructor
     */
    DeadlineSettings();
    
    /**
     * Calculate expected delivery time for remaining frames
     * 
     * @param last_frame_completion_time_ms Time taken to complete the last frame in ms
     * @param remaining_bytes_kb Remaining bytes to be transmitted in KB
     * @param link_bandwidths Map of link_id to bandwidth in Mbps
     * @param link_latencies Map of link_id to latency in ms
     * @param link_buffer_bytes Map of link_id to buffer occupancy in bytes
     * @param selected_links Vector of selected link IDs for transmission
     * @param chunk_sizes Vector of chunk sizes in KB assigned to each link
     * @return Expected delivery time in milliseconds
     */
    double calculateExpectedDeliveryTime(
        const std::map<int, double>& frame_bytes,
        const std::map<int, double>& link_bandwidths,
        const std::map<int, double>& link_latencies,
        const std::map<int, double>& link_buffer_bytes);
    
    /**
     * Calculate optimal deadline based on frame history
     * 
     * @param last_frame_completion_time_ms Time taken to complete the last frame in ms
     * @param avg_frame_size_kb Average frame size in KB
     * @param target_fps Target frames per second
     * @return Optimal deadline in milliseconds
     */
    double calculateOptimalDeadline(
        double last_frame_completion_time_ms,
        double avg_frame_size_kb, 
        double target_fps);
    
    /**
     * Determine if multilink transmission should be used
     * 
     * @param expected_delivery_time_ms Expected delivery time using single link
     * @param deadline_ms Current deadline
     * @param deadline_threshold Threshold ratio to trigger multilink (e.g., 0.9)
     * @return True if multilink should be used
     */
    bool shouldUseMultilink(
        double expected_delivery_time_ms,
        double deadline_ms,
        double deadline_threshold);
    
    /**
     * Calculate buffer delay based on buffer occupancy and link bandwidth
     * 
     * @param buffer_bytes Buffer occupancy in bytes
     * @param bandwidth_mbps Link bandwidth in Mbps
     * @return Buffer delay in milliseconds
     */
    double calculateBufferDelay(double buffer_bytes, double bandwidth_mbps);

private:
    // Minimum deadline to prevent unrealistic values
    const double MIN_DEADLINE_MS = 10.0;
    
    // Maximum deadline to prevent excessive delays
    const double MAX_DEADLINE_MS = 500.0;
    
    // Jitter buffer size in frames
    const double JITTER_BUFFER_FRAMES = 2.0;
};

} // namespace ric

#endif // DEADLINE_SETTING_HPP