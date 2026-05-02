#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <map>

struct PDCPConfig {
    uint32_t reordering_timer_ms = 100;
    uint32_t discard_timer_ms = 2000;
    bool duplicate_elimination = true;
    uint32_t sn_size = 18; // Sequence number size in bits
};

struct PDCPPacket {
    uint32_t sn; // Sequence number
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint32_t priority;
    bool requires_reliability;
};

class PDCPLayer {
public:
    explicit PDCPLayer(const PDCPConfig& config);
    
    // Submit packet from upper layer (e.g., WebRTC)
    void submit_packet(const std::vector<uint8_t>& data, uint32_t priority);
    
    // Receive packet from RLC layer
    void receive_from_rlc(const std::vector<uint8_t>& data, int path_id);
    
    // Start/Stop PDCP operations
    void start();
    void stop();

private:
    void tx_process();
    void rx_process();
    uint32_t select_best_path(const PDCPPacket& packet);
    
    PDCPConfig config_;
    bool running_;
    uint32_t next_sn_;
    
    std::mutex tx_mutex_;
    std::mutex rx_mutex_;
    std::condition_variable tx_cv_;
    std::condition_variable rx_cv_;
    
    std::queue<PDCPPacket> tx_queue_;
    std::map<uint32_t, PDCPPacket> rx_window_;
    
    std::vector<std::thread> threads_;
};