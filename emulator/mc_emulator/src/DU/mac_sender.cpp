#include "mac_sender.hpp"
#include <algorithm>
#include <utility>

MacSenderModule::MacSenderModule(std::vector<QueueHandle> rlcQueueHandles, double slotDurationMs)
    : rlcQueues(std::move(rlcQueueHandles)),
      running(false),
      slotDuration(slotDurationMs),
      packetsSent(0),
      totalLatency(0),
      bytesSent(0),
      lastSlotTime(std::chrono::steady_clock::now()),
      lastThroughputCalcTime(std::chrono::steady_clock::now()),
      currentThroughputBps(0) {
    std::sort(rlcQueues.begin(), rlcQueues.end(),
              [](const QueueHandle& a, const QueueHandle& b) {
                  return a.priority > b.priority;
              });

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

double MacSenderModule::getThroughputBps() const {
    // Return the current calculated throughput in bytes per second
    return currentThroughputBps.load();
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
        cv.wait_for(lock, std::chrono::microseconds(static_cast<long long>(slotDuration*1000)), 
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

    uint32_t sent_bytes = 0;
    size_t lastQueueIndex = 0; // Track last served queue for round-robin within priority level

    // Group queues by priority for better fairness
    std::map<uint8_t, std::vector<size_t>, std::greater<uint8_t>> priorityGroups;
    for (size_t i = 0; i < rlcQueues.size(); i++) {
        priorityGroups[rlcQueues[i].priority].push_back(i);
    }

    // Try to dequeue and send as many packets as possible within the available bytes
    while (running.load() && availableBytes > 0) {
        bool packetSentThisRound = false;

        // Process each priority level from highest to lowest
        for (auto& priorityGroup : priorityGroups) {
            if (availableBytes == 0) break;

            uint8_t priority = priorityGroup.first;
            auto& queueIndices = priorityGroup.second;

            // Within same priority, use round-robin for fairness
            bool foundPacketInPriority = false;
            size_t attempts = 0;

            while (attempts < queueIndices.size() && availableBytes > 0) {
                // Find next queue in this priority group with packets
                bool foundQueue = false;
                size_t queueIndex = 0;

                for (size_t offset = 0; offset < queueIndices.size(); offset++) {
                    size_t idx = (lastQueueIndex + offset) % queueIndices.size();
                    queueIndex = queueIndices[idx];

                    if (rlcQueues[queueIndex].module && rlcQueues[queueIndex].module->hasPackets()) {
                        lastQueueIndex = (idx + 1) % queueIndices.size(); // Update for next round
                        foundQueue = true;
                        break;
                    }
                }

                if (!foundQueue) {
                    break; // No more packets in this priority level
                }

                const auto& queue = rlcQueues[queueIndex];
                auto packet = queue.module->dequeuePacket(availableBytes);

                if (!packet) {
                    attempts++;
                    continue;
                }

                if (packet->size > availableBytes) {
                    // Packet does not fit in remaining budget; skip for this round
                    attempts++;
                    continue;
                }

                // Queue metadata is already embedded in serialized RLC segments

                bool sent = phyCallback(packet->data.data(), packet->size);

                if (!sent) {
                    LOG_WARN("[MAC Sender] PHY layer couldn't send packet from queue "
                             << static_cast<int>(queue.queueId) << " (priority "
                             << static_cast<int>(queue.priority) << ")");
                    return;
                }

                LOG_INFO("[MAC Sender] Successfully sent packet from queue "
                         << static_cast<int>(queue.queueId)
                         << " (priority=" << static_cast<int>(queue.priority)
                         << ", size=" << packet->size << " bytes)"
                         << " [remaining_bandwidth=" << (availableBytes - packet->size) << " bytes]");

                packetsSent.fetch_add(1, std::memory_order_relaxed);
                availableBytes -= packet->size;
                sent_bytes += packet->size;
                packetSentThisRound = true;
                foundPacketInPriority = true;

                if (packetsSent.load() % 1000 == 0) {
                    LOG_INFO("[MAC Sender] Processed 1000 packets in one slot, yielding");
                }

                // Continue within this priority level for better latency
                attempts = 0; // Reset attempts counter since we successfully sent a packet
            }

            // If we found packets in this priority level, don't check lower priorities
            if (foundPacketInPriority) {
                break;
            }
        }

        if (!packetSentThisRound) {
            break; // No queue could provide a packet within the remaining budget
        }
    }

    //LOG_DEBUG("[MAC Sender] Slot completed: sent " << sent_bytes << " bytes");
}