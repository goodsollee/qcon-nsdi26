#pragma once

struct MACConfig {
    uint32_t scheduling_period_ms = 1;
    uint32_t harq_processes = 8;
    uint32_t max_tb_size = 1500; // Maximum transport block size
};

struct MACFrame {
    uint32_t harq_id;
    std::vector<RLCSegment> segments;
    uint32_t total_size;
    uint8_t mcs; // Modulation and coding scheme
    bool new_transmission;
};

class MACLayer {
public:
    explicit MACLayer(const MACConfig& config, int path_id);
    
    // Handle segments from RLC
    void receive_from_rlc(const RLCSegment& segment);
    
    // Handle feedback from PHY
    void receive_feedback_from_phy(uint32_t harq_id, bool ack);
    
    // Start/Stop MAC operations
    void start();
    void stop();

private:
    void scheduler_process();
    void harq_process();
    MACFrame create_transport_block();
    
    MACConfig config_;
    int path_id_;
    bool running_;
    
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    std::queue<RLCSegment> tx_queue_;
    std::vector<std::queue<MACFrame>> harq_queues_;
    
    std::vector<std::thread> threads_;
};