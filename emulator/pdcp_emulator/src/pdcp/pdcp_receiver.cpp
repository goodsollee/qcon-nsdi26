#include "pdcp_receiver.hpp"
#include "log.h" // Provide LOG_INFO, LOG_WARN, etc.

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
      reorderTimeoutMs(cfg.receiver.reorderTimeoutMs > 0 ? cfg.receiver.reorderTimeoutMs : PDCP_DEFAULT_REORDER_TIMEOUT_MS),
      deliveredCount(0),
      droppedCount(0),
      stopThread(false),
      async_logger(cfg.common.logFoldername + "/pdcp_receiver.csv") 
{
    LOG_INFO("[PDCP Receiver] Initialized with reordering timeout "
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
    
    LOG_INFO("[PDCP Receiver] Cleaned up, delivered "
             << deliveredCount.load()
             << " packets, dropped "
             << droppedCount.load()
             << " packets");
}

void PdcpReceiver::timerThreadFunc() {
    LOG_INFO("[PDCP Receiver] Timer thread started");

    while (true) {
        // 1) Wait until stopThread == true OR timerRunning == true
        std::unique_lock<std::mutex> lock(timerMutex);
        timerCV.wait(lock, [this]() {
            return stopThread || timerRunning.load(std::memory_order_relaxed);
        });

        if (stopThread) {
            LOG_INFO("[PDCP Receiver] Timer thread stopping");
            break;
        }

        LOG_INFO("[PDCP Receiver] Timer is on running...");

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
            LOG_INFO("[PDCP Receiver] Timer thread stopping");
            break;
        }

        // If we woke because timerRunning was turned off, skip the reorder check
        if (!timerRunning.load(std::memory_order_relaxed)) {
            LOG_INFO("[PDCP Receiver] Timer stopped during waiting");
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
        LOG_INFO("[PDCP Receiver] Started reordering timer");
    }
}

void PdcpReceiver::stopReorderingTimer() {
    // Atomically set the flag to false; we only notify if it was previously true
    bool wasRunning = timerRunning.exchange(false, std::memory_order_relaxed);
    LOG_INFO("stopReorderingTimer() called. wasRunning=" << wasRunning);
    if (wasRunning) {
        // We lock the same mutex we use in timerThreadFunc()
        // Wake up the timer thread so it knows the timer is off
        timerCV.notify_one();
        LOG_INFO("stopReorderingTimer() called and notify_one()");
    }
    LOG_INFO("[PDCP Receiver] Stopped reordering timer");
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
                    LOG_WARN("[PDCP Receiver] Updating nextExpected from " 
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
        deliveryCallback();
    }
}

// Buffering packets
size_t PdcpReceiver::processPacket(unsigned char* packet, size_t len)
{
    // 1) Must have at least 20 bytes of outer IP + PDCP header
    if (len < sizeof(ipv4_hdr) + sizeof(pdcp_hdr)) {
        return len; // Not a PDCP-encapped packet, pass through
    }

    // 2) Parse the outer IP
    auto* out_iph = reinterpret_cast<ipv4_hdr*>(packet);
    size_t outerLen = static_cast<size_t>(out_iph->ihl) * 4; // e.g. 20
    if (outerLen < 20 || outerLen > len) {
        return len; // Invalid outer IP, pass
    }

    if (out_iph->protocol != 4) {
        // Not an IP-in-IP packet => no PDCP
        return len; // pass through
    }
    
    // 3) PDCP header starts after outer IP
    unsigned char* pdcpPtr = packet + outerLen;
    size_t remain = len - outerLen; // PDCP + inner IP length
    if (remain < sizeof(pdcp_hdr)) {
        return len; // too short for PDCP
    }

    auto* pdcpH = reinterpret_cast<pdcp_hdr*>(pdcpPtr);
    uint32_t epoch = ntohl(pdcpH->epoch);
    uint32_t seq   = ntohl(pdcpH->sequence_number);

    LOG_DEBUG("[PDCP Receiver] Received PDCP seq=" << seq << " Next expected=" << nextExpected.load(std::memory_order_relaxed));

    // Extract the send_timestamp from PDCP header (big-endian -> host)
    uint64_t beTime;
    std::memcpy(&beTime, &pdcpH->send_timestamp, sizeof(uint64_t));
    uint64_t pdcpSentTime = be64toh(beTime);

    // 4) The "inner IP" starts after PDCP
    unsigned char* innerIP = pdcpPtr + sizeof(pdcp_hdr);
    size_t innerLen = remain - sizeof(pdcp_hdr);

    if (innerLen > PDCP_MAX_PACKET_SIZE) {
        LOG_WARN("[PDCP Receiver] Inner IP too large: "
                 << innerLen << " bytes");
        droppedCount.fetch_add(1, std::memory_order_relaxed);
        return 0; // drop
    }

    // Mark local arrival
    uint64_t arrival_ms = getNowMs();

    uint32_t idx = seq % PDCP_MAX_REORDER_WINDOW;

    // Old sequence handling:
    // If this sequence is very old (well before nextExpected), deliver immediately
    if (seq < nextExpected.load(std::memory_order_relaxed) && epoch <= highestEpoch.load(std::memory_order_relaxed)) {
        LOG_WARN("[PDCP Receiver] Received very old seq=" << seq <<" epoch: "<<epoch<<" "<<nextExpected.load(std::memory_order_relaxed)<< ", delivering immediately");
        std::memcpy(packet, innerIP, innerLen);
        deliveredCount.fetch_add(1, std::memory_order_relaxed);

        // -----------------
        // Log PDCP/TCP info
        // -----------------
        {
            // Log final with depart_time = now
            uint64_t depart_ms = arrival_ms;

            // Attempt to parse the TCP seq
            uint32_t tcpSeq = 0;
            bool isTcp = pdcp::analyzeTcp(innerIP, innerLen, tcpSeq);

            // Build log line: pdcp_seq, tcp_seq, pdcp_sent_time, arrive_time, depart_time
            std::stringstream ss;
            ss << seq << ","
            << (isTcp ? std::to_string(tcpSeq) : "N/A") << ","
            << pdcpSentTime << ","
            << arrival_ms << ","
            << depart_ms <<","
            << innerLen;
            async_logger.logLine(ss.str());
        }

        return innerLen;
    } else {
        // 5) Add to reordering buffer
        if (packetPresent[idx]) {
            // Already present => duplicate
            LOG_WARN("[PDCP Receiver] Duplicate PDCP seq=" << seq);
            droppedCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        /*
        if (seq == nextExpected.load(std::memory_order_relaxed)) {

            std::unique_lock<std::mutex> lock(bufferMutex);
            // In-order packet
            LOG_DEBUG("[PDCP Receiver] Delivering in-order PDCP seq=" << seq);

            // Instead of analyzing `innerIP` directly, copy to a local vector first:
            std::vector<unsigned char> tempData(innerIP, innerIP + innerLen);

            uint64_t depart_ms = arrival_ms;
            uint32_t tcpSeq = 0;
            // Then parse from `tempData`, just like reorder uses `pkt.data`:
            bool isTcp = pdcp::analyzeTcp(tempData.data(), tempData.size(), tcpSeq);

            // If you still need to pass the packet upward or modify the original buffer:
            std::memcpy(packet, tempData.data(), tempData.size());;
            nextExpected.fetch_add(1, std::memory_order_relaxed);
            deliveredCount.fetch_add(1, std::memory_order_relaxed);

            std::stringstream ss;
            ss << seq << ","
            << (isTcp ? std::to_string(tcpSeq) : "N/A") << ","
            << pdcpSentTime << ","
            << arrival_ms << ","
            << depart_ms <<","
            << innerLen;
            async_logger.logLine(ss.str());

            if (timerRunning.load(std::memory_order_relaxed) && deliveryCallback) {
                lock.unlock();
                deliveryCallback();
            }

            return innerLen;
        } else {
            LOG_DEBUG("[PDCP Receiver] out-of-order PDCP seq=" << seq);
            startReorderingTimer();
        }
        */
        std::lock_guard<std::mutex> lock(bufferMutex);
        LOG_DEBUG("[PDCP Receiver] Buffered PDCP seq=" << seq << " Epoch=" <<epoch<< " highestReceived=" << highestReceived.load(std::memory_order_relaxed));
        PdcpPacket& pkt = packets[idx];
        pkt.data.assign(innerIP, innerIP + innerLen);
        pkt.epoch = epoch;
        pkt.sequenceNumber = seq;
        clock_gettime(CLOCK_MONOTONIC, &pkt.timestamp);
        pkt.inUse = true;

        // store the times in ms
        pkt.pdcpSentTime = pdcpSentTime;
        pkt.arrivalTime  = arrival_ms;

        packetPresent[idx] = true;
    }

    if (epoch == highestEpoch.load(std::memory_order_relaxed)) {
        if (seq > highestReceived.load(std::memory_order_relaxed)) {
            highestReceived.store(seq, std::memory_order_relaxed);
        }
    } else if (epoch > highestEpoch.load(std::memory_order_relaxed)) {
        highestEpoch.store(epoch, std::memory_order_relaxed);
        highestReceived.store(seq, std::memory_order_relaxed);
        LOG_WARN("[PDCP Receiver] Updated epoch" << epoch << " seq=" << seq);
    } else if (epoch < highestEpoch.load(std::memory_order_relaxed)) {
        LOG_WARN("[PDCP Receiver] Received old epoch=" << epoch << " highestEpoch=" << highestEpoch.load(std::memory_order_relaxed));
    }

    if (deliveryCallback) {
        deliveryCallback();
    }
    // Otherwise, not delivered yet
    return 0;
}

// Deliver pakcet and detect unordered packets
bool PdcpReceiver::deliverPacket(unsigned char* packetOut, size_t& lenOut) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    uint32_t startSeq = nextExpected.load(std::memory_order_relaxed);
    uint32_t maxSeq   = highestReceived.load(std::memory_order_relaxed);
    bool delivered    = false;

    LOG_DEBUG("[PDCP Receiver] Delivering packets from " 
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

        std::stringstream ss;
        ss << pkt.sequenceNumber << ","
            << (isTcp ? std::to_string(tcpSeq) : "N/A") << ","
            << pkt.pdcpSentTime << ","
            << pkt.arrivalTime << ","
            << depart_ms << ","
            << pkt.data.size();
        async_logger.logLine(ss.str());

        delivered = true;
        
        // Debug output every 100 deliveries
        LOG_DEBUG("[PDCP Receiver] Delivered in-sequence packet "
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
