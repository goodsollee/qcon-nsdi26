#include "mac_receiver.hpp"

MacReceiverModule::MacReceiverModule(std::shared_ptr<RlcReceiverModule> rlcModule, const MacReceiverConfig& cfg) 
    : rlc(rlcModule),
      packetsProcessed(0),
      bytesProcessed(0),
      running(false) {
    
    configureUplinkScheduling(cfg.txOpportunityInterval, cfg.txDelay);
    
    LOG_INFO("[MAC Receiver] Initialized");
}

MacReceiverModule::~MacReceiverModule() {
    stop();
    
    // Clean up any remaining PDUs in the queue
    QueuedStatusPdu* pdu;
    while (txQueue.try_dequeue(pdu)) {
        delete pdu;
    }
    
    // Clean up pending PDUs
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto pdu : pendingPdus) {
            delete pdu;
        }
        pendingPdus.clear();
    }
    
    LOG_INFO("[MAC Receiver] Cleaned up. Packets processed: " << packetsProcessed.load() 
            << ", Bytes processed: " << bytesProcessed.load());
}

void MacReceiverModule::start() {
    if (running.load()) return;
    
    running.store(true);
    lastUpdateTime = std::chrono::steady_clock::now();
    
    // Start the scheduling and transmission threads
    opportunityThread = std::thread(&MacReceiverModule::opportunityThreadFunction, this);
    transmissionThread = std::thread(&MacReceiverModule::transmissionThreadFunction, this);
    
    LOG_INFO("[MAC Receiver] Started");
}

void MacReceiverModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    // Wake up both threads
    txCondition.notify_all();
    
    // Wait for threads to finish
    if (opportunityThread.joinable()) {
        opportunityThread.join();
    }
    
    if (transmissionThread.joinable()) {
        transmissionThread.join();
    }
    
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

void MacReceiverModule::setStatusPduCallback(std::function<void(const RlcStatusPdu&)> callback) {
    statusPduCallback = callback;
    LOG_INFO("[MAC Receiver] Status PDU callback set");
}

bool MacReceiverModule::sendStatusPdu(const RlcStatusPdu& status_pdu) {
    if (!running.load()) {
        LOG_WARN("[MAC Receiver] Attempted to send Status PDU while not running");
        return false;
    }
    
    LOG_DEBUG("[MAC Receiver] Queueing Status PDU with ACK SN: " << status_pdu.ack_sn);
    
    // Create queued PDU with current time + delay as deadline
    auto queuedPdu = new QueuedStatusPdu(status_pdu, txDelay);
    
    // Add to transmission queue
    if (txQueue.enqueue(queuedPdu)) {
        // Wake up transmission thread
        txCondition.notify_one();
        return true;
    } else {
        LOG_ERROR("[MAC Receiver] Failed to enqueue Status PDU, queue may be full");
        delete queuedPdu;
        return false;
    }
}

void MacReceiverModule::configureUplinkScheduling(uint32_t txOpportunityIntervalMs,
                               uint32_t txDelayMs) {
    this->txOpportunityInterval = std::chrono::milliseconds(txOpportunityIntervalMs);
    this->txDelay = std::chrono::milliseconds(txDelayMs);
    LOG_INFO("[MAC Receiver] Uplink scheduling configured: Opportunity interval=" 
            << txOpportunityIntervalMs << "ms, Transmission delay=" 
            << txDelayMs << "ms");
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

bool MacReceiverModule::immediateUplinkResource() {
    if (!running.load()) {
        LOG_WARN("[MAC Receiver] Attempted immediate uplink while not running");
        return false;
    }
    
    LOG_INFO("[MAC Receiver] Immediate uplink resource requested");
    
    // Process immediate uplink for all queued PDUs
    return processImmediateUplink();
}

void MacReceiverModule::opportunityThreadFunction() {
    LOG_INFO("[MAC Receiver] Opportunity thread started");
    
    auto nextTxOpportunity = std::chrono::steady_clock::now() + txOpportunityInterval;
    
    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        
        // Check if it's time for a transmission opportunity
        if (now >= nextTxOpportunity) {
            processTxOpportunity();
            nextTxOpportunity = now + txOpportunityInterval;
        }
        
        // Sleep until next transmission opportunity
        {
            std::unique_lock<std::mutex> lock(txMutex);
            txCondition.wait_until(lock, nextTxOpportunity, [this, nextTxOpportunity]() {
                return !running.load() || std::chrono::steady_clock::now() >= nextTxOpportunity;
            });
        }
    }
    
    LOG_INFO("[MAC Receiver] Opportunity thread finished");
}

void MacReceiverModule::transmissionThreadFunction() {
    LOG_INFO("[MAC Receiver] Transmission thread started");
    
    while (running.load()) {
        // Check and transmit any PDUs that have reached their deadline
        checkAndTransmitPendingPdus();
        
        // Calculate next wake-up time
        auto nextWakeup = std::chrono::steady_clock::now() + txDelay;
        
        // Determine if we have PDUs to process and when the next one is due
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (!pendingPdus.empty()) {
                // Find PDU with earliest deadline
                if (!pendingPdus.empty()) {
                    auto earliestDeadline = pendingPdus[0]->deadlineTime;
                    
                    // Find minimum deadline manually
                    for (size_t i = 1; i < pendingPdus.size(); i++) {
                        if (pendingPdus[i]->deadlineTime < earliestDeadline) {
                            earliestDeadline = pendingPdus[i]->deadlineTime;
                        }
                    }
                    
                    // Wake up at the earliest deadline time
                    if (earliestDeadline < nextWakeup) {
                        nextWakeup = earliestDeadline;
                    }
                }
            } else {
                // No pending PDUs, sleep for longer to save CPU
                nextWakeup = std::chrono::steady_clock::now() + txDelay;
            }
        }
        
        // Sleep until next deadline or until new PDUs are added
        {
            std::unique_lock<std::mutex> lock(txMutex);
            txCondition.wait_until(lock, nextWakeup, [this]() {
                return !running.load();
            });
        }
    }
    
    LOG_INFO("[MAC Receiver] Transmission thread finished");
}

void MacReceiverModule::processTxOpportunity() {
    LOG_DEBUG("[MAC Receiver] Processing transmission opportunity");
    
    // Count of PDUs moved from queue to pending
    size_t pdusGranted = 0;
    
    // Dequeue PDUs and add them to pending list with deadlines
    QueuedStatusPdu* pdu;
    while (txQueue.try_dequeue(pdu)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingPdus.push_back(pdu);
        pdusGranted++;
    }
    
    if (pdusGranted > 0) {
        LOG_DEBUG("[MAC Receiver] Granted transmission opportunity to " << pdusGranted << " PDUs");
        // Wake up transmission thread to process the pending PDUs
        txCondition.notify_one();
    } else {
        LOG_DEBUG("[MAC Receiver] No PDUs in queue for this opportunity");
    }
}

void MacReceiverModule::checkAndTransmitPendingPdus() {
    auto now = std::chrono::steady_clock::now();
    std::vector<QueuedStatusPdu*> readyPdus;
    
    // Find PDUs that have reached their deadline
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        
        auto it = pendingPdus.begin();
        while (it != pendingPdus.end()) {
            if ((*it)->deadlineTime <= now) {
                // This PDU is ready for transmission
                readyPdus.push_back(*it);
                it = pendingPdus.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Transmit all PDUs that are ready
    for (auto pdu : readyPdus) {
        transmitStatusPdu(pdu);
    }
    
    if (!readyPdus.empty()) {
        LOG_DEBUG("[MAC Receiver] Transmitted " << readyPdus.size() << " PDUs in batch");
    }
}

bool MacReceiverModule::processImmediateUplink() {
    LOG_DEBUG("[MAC Receiver] Processing immediate uplink resource");
    
    bool transmitted = false;

    // First, process all PDUs in the transmission queue
    QueuedStatusPdu* pdu;
    size_t queuePdusCount = 0;
    
    while (txQueue.try_dequeue(pdu)) {
        transmitStatusPdu(pdu);
        queuePdusCount++;
        transmitted = true;
    }
    
    if (queuePdusCount > 0) {
        LOG_DEBUG("[MAC Receiver] Immediately transmitted " << queuePdusCount << " PDUs from queue");
    }
    
    // Then, process all pending PDUs immediately
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        LOG_DEBUG("[MAC Receiver] Processing " << pendingPdus.size() << " pending PDUs for immediate transmission");
        
        // Move all PDUs to temp vector to avoid issues with iteration during erasure
        std::vector<QueuedStatusPdu*> tempPdus;
        tempPdus.swap(pendingPdus);
        
        // Transmit each PDU
        for (auto pdu : tempPdus) {
            transmitStatusPdu(pdu);
            transmitted = true;
        }
    }
    
    return transmitted;
}

void MacReceiverModule::transmitStatusPdu(QueuedStatusPdu* pdu) {
    if (running.load() && statusPduCallback) {
        
        statusPduCallback(pdu->pdu);
    }
    
    // Clean up
    delete pdu;
}