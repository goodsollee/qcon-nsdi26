#include "mac_receiver.hpp"

MacReceiverModule::MacReceiverModule(std::shared_ptr<RlcReceiverModule> rlcModule) 
    : rlc(rlcModule),
      packetsProcessed(0),
      bytesProcessed(0),
      running(false) {
    
    LOG_INFO("[MAC Receiver] Initialized");
}

MacReceiverModule::~MacReceiverModule() {
    stop();
    LOG_INFO("[MAC Receiver] Cleaned up. Packets processed: " << packetsProcessed.load() 
            << ", Bytes processed: " << bytesProcessed.load());
}

void MacReceiverModule::start() {
    if (running.load()) return;
    
    running.store(true);
    lastUpdateTime = std::chrono::steady_clock::now();
    
    LOG_INFO("[MAC Receiver] Started");
}

void MacReceiverModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    LOG_INFO("[MAC Receiver] Stopped");
}

bool MacReceiverModule::processReceivedPacket(const unsigned char* packet, size_t len) {
    if (!running.load()) {
        LOG_WARN("[MAC Receiver] Received packet while not running, dropping");
        return false;
    }
    
    // Update statistics
    bytesProcessed.fetch_add(len, std::memory_order_relaxed);
    packetsProcessed.fetch_add(1, std::memory_order_relaxed);
    
    LOG_DEBUG("[MAC Receiver] Processing packet of size " << len << " bytes");
    
    // Process RLC segment - forwarding to RLC layer
    if (rlc) {
        return rlc->processSegment(packet, len);
    }
    
    LOG_WARN("[MAC Receiver] No RLC module set, packet dropped");
    return false;
}

void MacReceiverModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
    
    LOG_INFO("[MAC Receiver] Delivery callback set");
}

uint32_t MacReceiverModule::getPacketsProcessed() const {
    return packetsProcessed.load(std::memory_order_relaxed);
}

uint32_t MacReceiverModule::getBytesProcessed() const {
    return bytesProcessed.load(std::memory_order_relaxed);
}

bool MacReceiverModule::deliverReassembledPackets() {
    if (!rlc || !deliveryCallback) {
        return false;
    }
    
    // Check if RLC has complete packets
    if (rlc->hasPackets()) {
        // Arbitrary large value since we're delivering complete packets
        constexpr size_t MAX_PACKET_SIZE = 65536;
        auto packet = rlc->dequeuePacket(MAX_PACKET_SIZE);
        
        if (packet) {
            deliveryCallback(packet->data.data(), packet->size);
            return true;
        }
    }
    
    return false;
}