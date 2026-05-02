#include "phy_sender.hpp"
#include "rlc_segmentation.hpp"
#include "pdcp_common.hpp"
#include <iostream>

PhySenderModule::PhySenderModule(const std::string& bwTraceFile, uint32_t updateIntervalMs) 
    : bandwidthTraceFile(bwTraceFile),
      updateInterval(updateIntervalMs),
      currentBandwidth(10000000), // Default 10 Mbps
      accumulatedBytes(0),
      bytesSent(0),
      running(false),
      lastUpdateTime(std::chrono::steady_clock::now()),
      lastTransmitTime(std::chrono::steady_clock::now()) {
    
    LOG_INFO("[PHY Sender] Initialized with default bandwidth: " << (currentBandwidth.load() / 1000000.0) 
             << " Mbps, update interval: " << updateIntervalMs << " ms");
    
    if (!bandwidthTraceFile.empty()) {
        LOG_INFO("[PHY Sender] Using bandwidth trace file: " << bandwidthTraceFile);
    }
}

PhySenderModule::~PhySenderModule() {
    stop();
    LOG_INFO("[PHY Sender] Cleaned up. Bytes sent: " << bytesSent.load());
}

void PhySenderModule::start() {
    if (running.load()) return;
    
    running.store(true);
    lastUpdateTime = std::chrono::steady_clock::now();
    lastTransmitTime = lastUpdateTime;
    
    // Only start the trace reading thread if a trace file is provided
    if (!bandwidthTraceFile.empty()) {
        bwUpdateThread = std::thread(&PhySenderModule::updateBandwidthFromTrace, this);
        LOG_INFO("[PHY Sender] Bandwidth update thread started");
    }
}

void PhySenderModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    if (bwUpdateThread.joinable()) {
        bwUpdateThread.join();
    }
    LOG_INFO("[PHY Sender] Bandwidth update thread stopped");
}

bool PhySenderModule::processPacket(const unsigned char* packet, size_t len) {
    if (!packet || len == 0) {
        return false;
    }

    // Create buffer for IP encapsulated packet
    std::vector<unsigned char> encap_buffer(sizeof(ipv4_hdr) + len);
    
    // Copy original packet to the encapsulated buffer (after IP header)
    memcpy(encap_buffer.data() + sizeof(ipv4_hdr), packet, len);
    
    // Fill in the IP header for encapsulation
    ipv4_hdr* ip_header = reinterpret_cast<ipv4_hdr*>(encap_buffer.data());
    memset(ip_header, 0, sizeof(ipv4_hdr));
    
    // Set basic IP header fields
    ip_header->version = 4;
    ip_header->ihl = 5;  // 5 * 4 = 20 bytes
    ip_header->ttl = 64;
    ip_header->protocol = 4;  // IPPROTO_IPIP (IP encapsulation)
    
    // Set fixed source and destination addresses as requested
    ip_header->saddr = htonl(0x0A640001);  // 10.100.0.1
    ip_header->daddr = htonl(0x09090909);  // 9.9.9.9
    
    // Set total length and ID
    ip_header->tot_len = htons(static_cast<uint16_t>(encap_buffer.size()));
    
    static uint16_t packet_id_counter = 0;
    ip_header->id = htons(packet_id_counter++);
    
    // Calculate IP checksum
    ip_header->check = 0;
    ip_header->check = pdcp::calculateIpChecksum(ip_header, sizeof(ipv4_hdr));
    
    LOG_DEBUG("[PHY Sender] IP encapsulated packet: original size=" << len 
             << ", encapsulated size=" << encap_buffer.size());
    
    // Get current time
    auto now = std::chrono::steady_clock::now();
    
    // Update bytes sent
    bytesSent.fetch_add(len, std::memory_order_relaxed);

    // Forward packet to the next hop
    if (deliveryCallback) {
        deliveryCallback(encap_buffer.data(), encap_buffer.size());
    }
    
    LOG_DEBUG("[PHY Sender] Sent packet of size " << encap_buffer.size() << " bytes");
    
    return true;
}

size_t PhySenderModule::getAvailableBytes(double slotDuration) {
    std::lock_guard<std::mutex> lock(mutex);
    
    // Calculate available bytes based on current bandwidth and slot duration
    size_t availableBytes = (slotDuration * currentBandwidth.load()) / 8 / 1000;
    
    //LOG_DEBUG("[PHY Sender] Available bytes: " << availableBytes << " for slot duration: " << slotDuration << "ms");
    
    return availableBytes;
}

void PhySenderModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
}

void PhySenderModule::setBandwidth(uint32_t bps) {
    std::lock_guard<std::mutex> lock(mutex);
    currentBandwidth.store(bps, std::memory_order_relaxed);
    LOG_INFO("[PHY Sender] Bandwidth set to " << (bps / 1000) << " kbps");
}

uint32_t PhySenderModule::getCurrentBandwidth() const {
    return currentBandwidth.load(std::memory_order_relaxed);
}

uint32_t PhySenderModule::getBytesSent() const {
    return bytesSent.load(std::memory_order_relaxed);
}

void PhySenderModule::updateBandwidthFromTrace() {
    std::ifstream file(bandwidthTraceFile);
    if (!file.is_open()) {
        LOG_ERROR("[PHY Sender] Error: Could not open bandwidth trace file: " << bandwidthTraceFile);
        return;
    }
    
    std::string line;
    while (running.load()) {
        if (std::getline(file, line)) {
            // Parse bandwidth from trace file (single value in kbps)
            try {
                int kbps = std::stoi(line);
                // Convert kbps to bps
                uint32_t bps = static_cast<uint32_t>(kbps) * 1000;
                setBandwidth(bps);
            } catch (const std::exception& e) {
                LOG_ERROR("[PHY Sender] Error parsing bandwidth value: " << line);
            }
            
            // Wait for updateInterval before reading next line
            std::this_thread::sleep_for(std::chrono::milliseconds(updateInterval));
        } else {
            // Reached end of file, wrap around to beginning
            file.clear();
            file.seekg(0, std::ios::beg);
            LOG_INFO("[PHY Sender] Reached end of trace file, restarting from beginning");
        }
    }
}