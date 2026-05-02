#pragma once

struct RLCConfig {
    uint32_t window_size = 512;
    uint32_t poll_retransmit_timer_ms = 200;
    uint32_t max_retransmissions = 3;
    uint32_t t_reassembly_ms = 1000;
};

struct RLCSegment {
    uint32_t sn;
    std::vector<uint8_t> data;
    bool is_retransmission;
    uint32_t retx_count;
    uint16_t segment_offset;
    uint16_t segment_length;
    bool final_segment;
};

class RLCLayer {
public:
    explicit RLCLayer(const RLCConfig& config, int path_id);
    
    // Handle packet from PDCP
    void receive_from_pdcp(const PDCPPacket& packet);
    
    // Handle segment from MAC
    void receive_from_mac(const std::vector<uint8_t>& data);
    
    // Start/Stop RLC operations
    void start();
    void stop();

private:
    void tx_process();
    void rx_process();
    void status_report_process();
    std::vector<RLCSegment> segment_pdcp_packet(const PDCPPacket& packet);
    
    RLCConfig config_;
    int path_id_;
    bool running_;
    
    std::mutex tx_mutex_;
    std::mutex rx_mutex_;
    std::condition_variable tx_cv_;
    
    std::queue<PDCPPacket> tx_queue_;
    std::map<uint32_t, std::vector<RLCSegment>> rx_segments_;
    
    std::vector<std::thread> threads_;
};
