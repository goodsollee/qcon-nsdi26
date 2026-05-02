 #include "phy_receiver.hpp"
#include "pdcp_common.hpp"

PhyReceiverModule::PhyReceiverModule() 
    : bytesReceived(0),
      packetsReceived(0),
      running(false) {
    
    LOG_INFO("[PHY Receiver] Initialized");
}

PhyReceiverModule::~PhyReceiverModule() {
    stop();
    LOG_INFO("[PHY Receiver] Cleaned up. Bytes received: " << bytesReceived.load() 
            << ", Packets received: " << packetsReceived.load());
}

void PhyReceiverModule::start() {
    if (running.load()) return;
    
    running.store(true);
    lastUpdateTime = std::chrono::steady_clock::now();
    
    LOG_INFO("[PHY Receiver] Started");
}

void PhyReceiverModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    LOG_INFO("[PHY Receiver] Stopped");
}

bool PhyReceiverModule::processReceivedPacket(const unsigned char* packet, size_t len) {
    if (!running.load()) {
        LOG_WARN("[PHY Receiver] Received packet while not running, dropping");
        return false;
    }
    
     // Check if we need to perform IP decapsulation first
    if (len < sizeof(ipv4_hdr)) {
        LOG_WARN("[PHY Receiver] Packet too small for IP header");
        return false;
    }

    const unsigned char* processed_packet = packet;
    size_t processed_len = len;
  
    // Update statistics
    bytesReceived.fetch_add(len, std::memory_order_relaxed);
    packetsReceived.fetch_add(1, std::memory_order_relaxed);
    
    LOG_DEBUG("[PHY Receiver] Received packet of size " << processed_len << " bytes");
    
    // Forward to MAC layer if available
    if (macReceiver) {
        return macReceiver->processReceivedPacket(processed_packet, processed_len);
    }
    
    // If no MAC layer, we need to forward directly to higher layer
    if (deliveryCallback) {
        deliveryCallback(processed_packet, processed_len);
        return true;
    }
    
    LOG_WARN("[PHY Receiver] No MAC layer or delivery callback set, packet dropped");
    return false;
}

void PhyReceiverModule::setMacReceiver(std::shared_ptr<MacReceiverModule> mac) {
    std::lock_guard<std::mutex> lock(mutex);
    macReceiver = mac;
    
    LOG_INFO("[PHY Receiver] MAC receiver module set");
}

void PhyReceiverModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    std::lock_guard<std::mutex> lock(mutex);
    deliveryCallback = callback;
    
    LOG_INFO("[PHY Receiver] Delivery callback set");
}

uint32_t PhyReceiverModule::getBytesReceived() const {
    return bytesReceived.load(std::memory_order_relaxed);
}

uint32_t PhyReceiverModule::getPacketsReceived() const {
    return packetsReceived.load(std::memory_order_relaxed);
}