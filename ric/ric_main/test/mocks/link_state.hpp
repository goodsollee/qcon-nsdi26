#pragma once

#include <string>
#include <chrono>
#include <queue>
#include <mutex>
#include <deque>

namespace ric {

// Simple struct to represent link state for testing
struct LinkState {
    int link_id;
    std::string component_id;
    std::string component_type;
    
    // Performance metrics
    double throughput_mbps;
    double delay_ms;
    double jitter_ms;
    double packet_loss_rate;
    double sinr_db;
    double buffer_occupancy;
    
    // Byte counts
    uint64_t rlc_acked_bytes;
    uint64_t pdcp_transmitted_bytes;
    
    // Status flags
    bool is_active;
    bool is_congested;
    
    // Timestamp of last update
    std::chrono::steady_clock::time_point last_update_time;
    
    LinkState() 
        : link_id(0), 
          throughput_mbps(0.0), 
          delay_ms(0.0), 
          jitter_ms(0.0), 
          packet_loss_rate(0.0), 
          sinr_db(0.0), 
          buffer_occupancy(0.0),
          rlc_acked_bytes(0),
          pdcp_transmitted_bytes(0),
          is_active(false),
          is_congested(false),
          last_update_time(std::chrono::steady_clock::now()) {}
};

} // namespace ric