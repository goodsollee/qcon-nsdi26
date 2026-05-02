// ran_emulator.cpp
#include "ran_emulator.h"

RANEmulator::RANEmulator() : running(false) {}

RANEmulator::~RANEmulator() {
    stop();
}

bool RANEmulator::init(const std::vector<LinkConfig>& link_configs) {
    std::lock_guard<std::mutex> lock(links_mutex);
    
    for (const auto& config : link_configs) {
        if (!add_link(config)) {
            std::cerr << "Failed to initialize link: " << config.name << std::endl;
            return false;
        }
    }
    
    return true;
}

bool RANEmulator::add_link(const LinkConfig& config) {
    std::unique_ptr<LinkState> link_state = std::make_unique<LinkState>();
    
    // Create socket
    link_state->socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (link_state->socket < 0) {
        std::cerr << "Failed to create socket for link: " << config.name << std::endl;
        return false;
    }
    
    // Load bandwidth trace
    if (!load_bandwidth_trace(config.trace_file, link_state->trace)) {
        std::cerr << "Failed to load trace file for link: " << config.name << std::endl;
        close(link_state->socket);
        return false;
    }
    
    // Initialize link state
    link_state->trace_index = 0;
    link_state->current_bandwidth_mbps = link_state->trace[0].bandwidth_mbps;
    link_state->current_latency_ms = link_state->trace[0].latency_ms;
    
    // Store link configuration
    int link_id = links.size();
    link_name_to_id[config.name] = link_id;
    links.push_back(std::move(link_state));
    
    return true;
}

void RANEmulator::start() {
    running = true;

    // Start PDCP layer threads
    worker_threads.emplace_back(&RANEmulator::pdcp_tx_process, this);
    worker_threads.emplace_back(&RANEmulator::pdcp_rx_process, this);
    
    // Start per-link threads
    for (size_t i = 0; i < links.size(); i++) {
        worker_threads.emplace_back(&RANEmulator::rlc_layer_process, this, i);
        worker_threads.emplace_back(&RANEmulator::mac_scheduler_process, this, i);
        worker_threads.emplace_back(&RANEmulator::phy_transmission_process, this, i);
    }
    
    // Start bandwidth control thread
    worker_threads.emplace_back(&RANEmulator::bandwidth_control_process, this);
}

void RANEmulator::pdcp_tx_process() {
    while (running) {
        std::unique_lock<std::mutex> lock(pdcp_mutex);
        pdcp_cv.wait(lock, [this] { return !pdcp_tx_queue.empty() || !running; });

        if (!running) break;

        if (!pdcp_tx_queue.empty()) {
            auto packet = pdcp_tx_queue.front();
            pdcp_tx_queue.pop();
            lock.unlock();

            // Select best link for packet
            int link_id = select_best_link(packet);
            
            // Segment packet and distribute to selected link
            segment_pdcp_packet(packet);
        }
    }
}

void RANEmulator::rlc_layer_process(int link_id) {
    auto& link = links[link_id];
    
    while (running) {
        std::unique_lock<std::mutex> lock(link->queue_mutex);
        link->queue_cv.wait(lock, [&] { 
            return !link->tx_queue.empty() || !running; 
        });

        if (!running) break;

        if (!link->tx_queue.empty()) {
            auto segment = link->tx_queue.front();
            link->tx_queue.pop();
            lock.unlock();

            // Apply link conditions (bandwidth, latency)
            double transmission_time = (segment.data.size() * 8.0) / 
                                    (link->current_bandwidth_mbps * 1e6);
            
            // Add link latency
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<int>(link->current_latency_ms * 1000)));
            
            // Forward to MAC layer
            // ... MAC layer processing ...
        }
    }
}

int RANEmulator::select_best_link(const PDCPPacket& packet) {
    // Simple round-robin selection for now
    static size_t current_link = 0;
    size_t selected_link = current_link;
    current_link = (current_link + 1) % links.size();
    
    // TODO: Implement more sophisticated link selection based on:
    // - Current bandwidth
    // - Latency requirements
    // - Queue length
    // - Link conditions
    
    return selected_link;
}

void RANEmulator::bandwidth_control_process() {
    auto start_time = std::chrono::steady_clock::now();

    while (running) {
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - start_time).count();

        // Update each link's conditions based on trace
        std::lock_guard<std::mutex> lock(links_mutex);
        for (auto& link : links) {
            while (link->trace_index < link->trace.size() && 
                   link->trace[link->trace_index].timestamp <= static_cast<uint64_t>(elapsed)) {
                // Update link conditions
                link->current_bandwidth_mbps = link->trace[link->trace_index].bandwidth_mbps;
                link->current_latency_ms = link->trace[link->trace_index].latency_ms;
                link->current_jitter_ms = link->trace[link->trace_index].jitter_ms;
                link->trace_index++;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}