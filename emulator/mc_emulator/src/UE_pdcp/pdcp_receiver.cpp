#include "pdcp_receiver.hpp"
#include "log.hpp" // Provide LOG_INFO, LOG_WARN, etc.
#include "pdcp_common.hpp"

#include <arpa/inet.h>
#include <cstring>

// Helper: convert current steady_clock to ms
static inline uint64_t getNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

PdcpReceiver::PdcpReceiver(const PdcpConfig& cfg) 
    : PdcpContext(PdcpRole::RECEIVER),
      packets(PDCP_MAX_REORDER_WINDOW),
      packetPresent(PDCP_MAX_REORDER_WINDOW, false),
      nextExpected(0),
      highestReceived(0),
      highestEpoch(0),
      timerRunning(false),
      reorderTimeoutMs(cfg.receiver.reordering_timeout_ms > 0 ? cfg.receiver.reordering_timeout_ms : PDCP_DEFAULT_REORDER_TIMEOUT_MS),
      deliveredCount(0),
      droppedCount(0),
      stopThread(false),
      async_logger(cfg.common.logFoldername + "/pdcp_receiver.csv") 
{
    LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Initialized with reordering timeout "
             << reorderTimeoutMs << " ms");
    
    // Write the CSV header line once
    async_logger.logLine("pdcp_seq,tcp_seq,pdcp_sent_time,pdcp_arrive_time,pdcp_depart_time,pkt_len");

    // Start timer thread
    timerThread = std::thread(&PdcpReceiver::timerThreadFunc, this);
}

PdcpReceiver::~PdcpReceiver() {
    // Stop timer thread
    {
        std::lock_guard<std::mutex> lock(timerMutex);
        stopThread = true;
    }
    timerCV.notify_one();
    
    if (timerThread.joinable()) {
        timerThread.join();
    }
    
    LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Cleaned up, delivered "
             << deliveredCount.load()
             << " packets, dropped "
             << droppedCount.load()
             << " packets");
}

void PdcpReceiver::timerThreadFunc() {
    LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Timer thread started");

    while (true) {
        // 1) Wait until stopThread == true OR timerRunning == true
        std::unique_lock<std::mutex> lock(timerMutex);
        timerCV.wait(lock, [this]() {
            return stopThread || timerRunning.load(std::memory_order_relaxed);
        });

        if (stopThread) {
            LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Timer thread stopping");
            break;
        }

        LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Timer is on running...");

        // 2) Now wait up to reorderTimeoutMs or until timerRunning is turned off
        bool done = timerCV.wait_for(
            lock,
            std::chrono::milliseconds(reorderTimeoutMs),
            [this]() {
                // Wake early if stopThread or timerRunning is turned off
                return stopThread || !timerRunning.load(std::memory_order_relaxed);
            }
        );

        // If we woke because stopThread is set
        if (stopThread) {
            LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Timer thread stopping");
            break;
        }

        // If we woke because timerRunning was turned off, skip the reorder check
        if (!timerRunning.load(std::memory_order_relaxed)) {
            LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Timer stopped during waiting");
            // Another thread called stopReorderingTimer()
            continue;
        }

        // Otherwise, we truly timed out => do the reorder check
        checkReorderingTimeout();
    }
}

void PdcpReceiver::startReorderingTimer() {
    if (!timerRunning.exchange(true, std::memory_order_relaxed)) {
        clock_gettime(CLOCK_MONOTONIC, &lastGapTime);
        timerCV.notify_one();
        LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Started reordering timer");
    }
}

void PdcpReceiver::stopReorderingTimer() {
    // Atomically set the flag to false; we only notify if it was previously true
    bool wasRunning = timerRunning.exchange(false, std::memory_order_relaxed);
    LOG_MODULE_INFO(MODULE_RECEIVER,"stopReorderingTimer() called. wasRunning=" << wasRunning);
    if (wasRunning) {
        // We lock the same mutex we use in timerThreadFunc()
        // Wake up the timer thread so it knows the timer is off
        timerCV.notify_one();
        LOG_MODULE_INFO(MODULE_RECEIVER,"stopReorderingTimer() called and notify_one()");
    }
    LOG_MODULE_INFO(MODULE_RECEIVER,"[PDCP Receiver] Stopped reordering timer");
}

void PdcpReceiver::checkReorderingTimeout() {
    bool doCallback = false;
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        
        uint32_t startSeq = nextExpected.load(std::memory_order_relaxed);
        uint32_t firstReceivedSeq = PDCP_SEQ_LIMIT; // Initialize to max value
        uint32_t maxSeq = highestReceived.load(std::memory_order_relaxed);

        // 1. Find first received packet after nextExpected
        for (uint32_t seq = startSeq; seq <= maxSeq; seq++) {
            uint32_t index = seq % PDCP_MAX_REORDER_WINDOW;
            if (packetPresent[index]) {
                firstReceivedSeq = seq;
                // 2. Set this as nextExpected
                if (firstReceivedSeq != nextExpected.load(std::memory_order_relaxed)) {
                    LOG_MODULE_ERROR(MODULE_RECEIVER,"[PDCP Receiver] Updating nextExpected from " 
                            << nextExpected.load(std::memory_order_relaxed) 
                            << " to " << firstReceivedSeq);
                    nextExpected.store(firstReceivedSeq, std::memory_order_relaxed);
                }
                if (deliveryCallback) {
                    doCallback = true;  // We'll call after unlocking
                }
                break;
            }
        }
    }
    if (doCallback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        deliveryCallback();
    }
}

// Buffering packets
size_t PdcpReceiver::processPacket(unsigned char* packet, size_t len)
{
    // Check for valid PDCP header
    if (len < sizeof(pdcp_hdr)) {
        return len; // Not a PDCP packet, pass through
    }
    
    // Parse PDCP header
    auto* pdcpH = reinterpret_cast<pdcp_hdr*>(packet);
    uint64_t epoch = ntohl(pdcpH->epoch);
    uint64_t seq = ntohl(pdcpH->sequence_number);
    uint8_t flags = pdcpH->flags;
    uint16_t reserved = ntohs(pdcpH->reserved);
    
    // Extract the send_timestamp from PDCP header
    uint64_t beTime;
    std::memcpy(&beTime, &pdcpH->send_timestamp, sizeof(uint64_t));
    uint64_t pdcpSentTime = be64toh(beTime);
    
    LOG_MODULE_DEBUG(MODULE_RECEIVER,"[PDCP Receiver] PDCP Header: "
            << "seq=" << seq 
            << " epoch=" << epoch
            << " flags=" << static_cast<int>(flags)
            << " reserved=" << reserved
            << " timestamp=" << pdcpSentTime
            << " Next expected=" << nextExpected.load(std::memory_order_relaxed));
    
    // The payload follows the PDCP header
    unsigned char* payload = packet + sizeof(pdcp_hdr);
    size_t payloadLen = len - sizeof(pdcp_hdr);
    
    if (payloadLen > PDCP_MAX_PACKET_SIZE) {
        LOG_MODULE_WARN(MODULE_RECEIVER,"[PDCP Receiver] Payload too large: " << payloadLen << " bytes");
        droppedCount.fetch_add(1, std::memory_order_relaxed);
        return 0; // drop
    }
    
    // Mark local arrival
    uint64_t arrival_ms = getNowMs();
    
    uint32_t idx = seq % PDCP_MAX_REORDER_WINDOW;
    
    // Check if packet is old and should be delivered immediately
    if (seq < nextExpected.load(std::memory_order_relaxed) && 
        epoch <= highestEpoch.load(std::memory_order_relaxed)) {
        LOG_MODULE_WARN(MODULE_RECEIVER,"[PDCP Receiver] Received very old seq=" << seq << ", delivering immediately");
        
        // Move payload to the beginning of the buffer, removing PDCP header
        std::memmove(packet, payload, payloadLen);
        deliveredCount.fetch_add(1, std::memory_order_relaxed);
        
        return payloadLen;
    } 
    else {
        // Check for duplicate
        if (nextExpected.load(std::memory_order_relaxed) >= seq + 1) {
            LOG_MODULE_WARN(MODULE_RECEIVER,"[PDCP Receiver] Duplicate PDCP seq=" << seq);
            droppedCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        
        // Buffer the packet for reordering
        std::lock_guard<std::mutex> lock(bufferMutex);
        LOG_MODULE_DEBUG(MODULE_RECEIVER,"[PDCP Receiver] Buffered PDCP seq=" << seq << " Epoch=" << epoch 
                << " highestReceived=" << highestReceived.load(std::memory_order_relaxed));
                
        PdcpPacket& pkt = packets[idx];
        pkt.data.assign(payload, payload + payloadLen);
        pkt.epoch = epoch;
        pkt.sequenceNumber = seq;
        clock_gettime(CLOCK_MONOTONIC, &pkt.timestamp);
        pkt.inUse = true;
        
        // Store timing information
        pkt.pdcpSentTime = pdcpSentTime;
        pkt.arrivalTime = arrival_ms;
        
        packetPresent[idx] = true;
    }
    
    // Update highest received sequence
    if (epoch == highestEpoch.load(std::memory_order_relaxed)) {
        if (seq > highestReceived.load(std::memory_order_relaxed)) {
            highestReceived.store(seq, std::memory_order_relaxed);
        }
    } 
    else if (epoch > highestEpoch.load(std::memory_order_relaxed)) {
        highestEpoch.store(epoch, std::memory_order_relaxed);
        highestReceived.store(seq, std::memory_order_relaxed);
        LOG_MODULE_DEBUG(MODULE_RECEIVER,"[PDCP Receiver] Updated epoch " << epoch << " seq=" << seq);
    }

    if (deliveryCallback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        // Call the delivery callback if set
        deliveryCallback();
    }
    
    // Return 0 to indicate we've handled the packet
    return 0;
}

// Deliver pakcet and detect unordered packets
bool PdcpReceiver::deliverPacket(unsigned char* packetOut, size_t& lenOut) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    uint32_t startSeq = nextExpected.load(std::memory_order_relaxed);
    uint32_t maxSeq   = highestReceived.load(std::memory_order_relaxed);
    bool delivered    = false;

    LOG_MODULE_DEBUG(MODULE_RECEIVER,"[PDCP Receiver] Delivering packets from " 
              << startSeq << " to " << maxSeq);

    // First, try to deliver packets in sequence 
    uint32_t index = startSeq % PDCP_MAX_REORDER_WINDOW;
    
    if (!packetPresent[index]) {
        // Gap detected - start reordering timer if it's not already running
        if (startSeq <= highestReceived.load(std::memory_order_relaxed)) {
            startReorderingTimer();
        }
        return delivered;
    }

    if (startSeq == maxSeq) {
        // Means we have no gap => we can stop the reorder timer
        {
            stopReorderingTimer();
        }
    }

    PdcpPacket& pkt = packets[index];
    
    // In-sequence delivery
    if (pkt.data.size() <= PDCP_MAX_PACKET_SIZE) {
        std::memcpy(packetOut, pkt.data.data(), pkt.data.size());
        lenOut = pkt.data.size();
        
        // Mark as delivered
        packetPresent[index] = false;
        nextExpected.fetch_add(1, std::memory_order_relaxed);
        deliveredCount.fetch_add(1, std::memory_order_relaxed);

        // Reset
        if (startSeq == PDCP_SEQ_LIMIT) {
            nextExpected.store(0, std::memory_order_relaxed);
        }

        // Now log: seq, tcp_seq, pdcpSentTime, arrivalTime, departTime

        uint64_t depart_ms = getNowMs();
        uint32_t tcpSeq = 0;
        bool isTcp = pdcp::analyzeTcp(pkt.data.data(), pkt.data.size(), tcpSeq);

        /*
        std::stringstream ss;
        ss << pkt.sequenceNumber << ","
            << (isTcp ? std::to_string(tcpSeq) : "N/A") << ","
            << pkt.pdcpSentTime << ","
            << pkt.arrivalTime << ","
            << depart_ms << ","
            << pkt.data.size();
        async_logger.logLine(ss.str());
        */
        delivered = true;
        
        // Debug output every 100 deliveries
        LOG_MODULE_DEBUG(MODULE_RECEIVER,"[PDCP Receiver] Delivered in-sequence packet "
                    << pkt.sequenceNumber 
                    << ", total delivered: "
                    << deliveredCount.load()
                    << ", next expected: "
                    << nextExpected.load());
        
        // Deliver only one packet per call
        return delivered;
    }

    return delivered;
}

void PdcpReceiver::getStats(uint32_t& nextExp, uint32_t& highestRcvd, 
                            uint32_t& delivered, uint32_t& dropped) const 
{
    nextExp     = nextExpected.load(std::memory_order_relaxed);
    highestRcvd = highestReceived.load(std::memory_order_relaxed);
    delivered   = deliveredCount.load(std::memory_order_relaxed);
    dropped     = droppedCount.load(std::memory_order_relaxed);
}
