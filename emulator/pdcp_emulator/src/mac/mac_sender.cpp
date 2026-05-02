#include "mac_sender.hpp"

MacSenderModule::MacSenderModule(std::shared_ptr<RlcSenderModule> rlcModule, double slotDurationMs) 
    : rlc(rlcModule),
      running(false),
      slotDuration(slotDurationMs),
      packetsSent(0),
      totalLatency(0),
      lastSlotTime(std::chrono::steady_clock::now()) {
    LOG_INFO("[MAC Sender] Initialized with slot duration: " << slotDurationMs << " ms");
}

MacSenderModule::~MacSenderModule() {
    stop();
    LOG_INFO("[MAC Sender] Cleaned up. Packets sent: " << packetsSent.load() 
            << ", Average latency: " << getAverageLatency() << " ms");
}

void MacSenderModule::start() {
    if (running.load()) return;
    
    running.store(true);
    schedulerThread = std::thread(&MacSenderModule::schedulerFunction, this);
    LOG_INFO("[MAC Sender] Scheduler started");
}

void MacSenderModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_one();
    }
    
    if (schedulerThread.joinable()) {
        schedulerThread.join();
    }
    LOG_INFO("[MAC Sender] Scheduler stopped");
}

void MacSenderModule::setPhyCallback(std::function<bool(const unsigned char*, size_t)> callback) {
    phyCallback = callback;
}

void MacSenderModule::setGetAvailableBytesCallback(std::function<size_t(double)> callback) {
    getAvailableBytesCallback = callback;
}

double MacSenderModule::getThroughput() const {
    // Simple throughput calculation - packets per second
    uint64_t durationSec = totalLatency.load() / 1000;
    if (durationSec == 0) return 0.0;
    return static_cast<double>(packetsSent.load()) / durationSec;
}

double MacSenderModule::getAverageLatency() const {
    // Average latency in milliseconds
    if (packetsSent.load() == 0) return 0.0;
    return static_cast<double>(totalLatency.load()) / packetsSent.load();
}

void MacSenderModule::schedulerFunction() {
    while (running.load()) {
        // Process all available packets in the current slot
        processSlot();
        
        // Wait for the next slot
        std::unique_lock<std::mutex> lock(mutex);
        lastSlotTime = std::chrono::steady_clock::now();
        cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(slotDuration)), 
                   [this]{ return !running.load(); });
    }
}

void MacSenderModule::processSlot() {
    // Skip processing if no PHY callback is set
    if (!phyCallback || !getAvailableBytesCallback) {
        return;
    }
    
    // Get available bytes from PHY layer
    size_t availableBytes = getAvailableBytesCallback(slotDuration);
    
    if (availableBytes == 0) {
        LOG_DEBUG("[MAC Sender] No bandwidth available in this slot");
        return;
    }
    
    //LOG_DEBUG("[MAC Sender] Processing slot with " << availableBytes << " available bytes");
    
    // Try to dequeue and send as many packets as possible within the available bytes
    while (rlc->hasPackets() && running.load()) {
        // Check front packet size
        size_t frontPacketSize = rlc->getFrontPacketSize();
        
        // Dequeue packet - will handle segmentation if needed
        auto packet = rlc->dequeuePacket(availableBytes);
        if (!packet) {
            // No more packets available or they're too large
            break;
        }
        
        // Calculate packet latency in buffer
        auto now = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - packet->timestamp).count();
        totalLatency.fetch_add(latency, std::memory_order_relaxed);
        
        // Send to PHY layer
        bool sent = phyCallback(packet->data.data(), packet->size);
        
        if (sent) {
            packetsSent.fetch_add(1, std::memory_order_relaxed);
            availableBytes -= packet->size;
            
            //LOG_DEBUG("[MAC Sender] Sent packet of size " << packet->size 
            //         << " bytes, remaining bytes: " << availableBytes
            //         << ", latency: " << latency << " ms");
        } else {
            // PHY layer couldn't send the packet
            LOG_WARN("[MAC Sender] PHY layer couldn't send packet");
            break;
        }
        
        // If we've sent too many packets in one slot, break to avoid blocking
        if (packetsSent.load() % 1000 == 0) {
            LOG_INFO("[MAC Sender] Processed 1000 packets in one slot, yielding");
            break;
        }
    }
}