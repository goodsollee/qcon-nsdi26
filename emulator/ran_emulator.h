// ran_emulator.h
#pragma once

#include <vector>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <netinet/ip.h>
#include <netinet/udp.h>

struct LinkConfig {
    std::string name;           // Link identifier
    std::string trace_file;     // Bandwidth trace file
    std::string local_ip;       // Local IP for this interface
    uint16_t local_port;        // Local port
    std::string remote_ip;      // Remote IP
    uint16_t remote_port;       // Remote port
    double base_latency_ms;     // Base latency for this link
};

struct LinkState {
    int socket;
    double current_bandwidth_mbps;
    double current_latency_ms;
    double current_jitter_ms;
    std::vector<BandwidthTrace> trace;
    size_t trace_index;
    std::queue<RLCSegment> tx_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
};

struct BandwidthTrace {
    uint64_t timestamp;
    double bandwidth_mbps;
    double latency_ms;
    double jitter_ms;
};

struct PDCPPacket {
    uint32_t sn;
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint16_t ue_id;
    bool is_last_packet;
    std::vector<uint8_t> original_ip_header; // Store original IP header for reconstruction
};

struct RLCSegment {
    uint16_t sn;
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint32_t pdcp_sn;
    bool is_last_segment;
    uint16_t link_id;          // Identify which link this segment belongs to
};

class RANEmulator {
public:
    RANEmulator();
    ~RANEmulator();

    // Initialize emulator with multiple links
    bool init(const std::vector<LinkConfig>& link_configs);
    
    // Start/Stop emulation
    void start();
    void stop();

    // Add new link at runtime
    bool add_link(const LinkConfig& config);
    
    // Remove link at runtime
    bool remove_link(const std::string& link_name);

    // Main packet injection function
    bool inject_packet(const std::vector<uint8_t>& packet_data);

private:
    // Layer processing functions
    void pdcp_tx_process();
    void pdcp_rx_process();
    void rlc_layer_process(int link_id);
    void mac_scheduler_process(int link_id);
    void phy_transmission_process(int link_id);

    // Bandwidth control
    void bandwidth_control_process();
    
    // Helper functions
    void segment_pdcp_packet(const PDCPPacket& packet);
    PDCPPacket reassemble_pdcp_packet(const std::vector<RLCSegment>& segments);
    bool load_bandwidth_trace(const std::string& trace_file, std::vector<BandwidthTrace>& trace);
    int select_best_link(const PDCPPacket& packet);
    void update_link_metrics();
    
    // Packet processing helpers
    std::vector<uint8_t> extract_ip_header(const std::vector<uint8_t>& packet);
    void reconstruct_packet(PDCPPacket& packet);

private:
    // Link management
    std::map<std::string, int> link_name_to_id;
    std::vector<std::unique_ptr<LinkState>> links;
    std::mutex links_mutex;

    // Packet queues
    std::queue<PDCPPacket> pdcp_tx_queue;
    std::map<uint32_t, std::vector<RLCSegment>> pdcp_rx_buffer;
    std::mutex pdcp_mutex;
    std::condition_variable pdcp_cv;

    // Thread management
    bool running;
    std::vector<std::thread> worker_threads;
    
    // Configuration
    static const int MAX_SEGMENT_SIZE = 1500;
    static const int BUFFER_SIZE = 65536;
};