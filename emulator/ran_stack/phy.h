#pragma once

#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <vector>
#include "../network_emulation/network_emulator.h"

// Forward declaration of MAC frame structure
struct MACFrame {
    uint32_t harq_id;
    std::vector<uint8_t> data;
    bool new_transmission;
};

class PHYLayer {
public:
    explicit PHYLayer(NetworkEmulator* emulator);
    ~PHYLayer();

    // Initialize PHY layer
    bool init();

    // Start/Stop PHY operations
    void start();
    void stop();

    // Interface with MAC layer
    void receive_from_mac(const MACFrame& frame);

    // Get current network conditions (for MAC scheduling decisions)
    struct NetworkConditions {
        double current_bandwidth_kbps;
        double current_latency_ms;
    };
    NetworkConditions get_network_conditions() const;

private:
    void transmission_process();
    
    NetworkEmulator* network_emulator_;
    bool running_;
    
    // Queue for frames from MAC layer
    std::queue<MACFrame> tx_queue_;
    std::mutex tx_mutex_;
    std::condition_variable tx_cv_;
    
    // Thread management
    std::vector<std::thread> worker_threads_;
};