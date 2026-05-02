#ifndef QOS_PROFILER_HPP
#define QOS_PROFILER_HPP

#include <cstdint>
#include <cstddef>
#include "pdcp_common.hpp"

/**
 * @brief QoS Profiler for determining packet queue assignments
 *
 * This class analyzes packet characteristics and assigns them to appropriate
 * RLC queues based on QoS requirements. Currently supports 4 queue levels:
 * - Queue 0: High priority (e.g., control traffic, VoIP)
 * - Queue 1: Medium-high priority (e.g., video streaming)
 * - Queue 2: Medium priority (e.g., web browsing)
 * - Queue 3: Low priority (e.g., background downloads)
 */
class QosProfiler {
public:
    /**
     * @brief QoS Classes for different traffic types
     */
    enum class QosClass : uint8_t {
        CONTROL = 0,    // High priority control traffic
        VOICE = 0,      // Voice/VoIP traffic
        VIDEO = 1,      // Video streaming
        DATA = 2,       // General data traffic
        BACKGROUND = 3  // Background/bulk traffic
    };

    /**
     * @brief Packet classification modes
     */
    enum class ClassificationMode : uint8_t {
        ROUND_ROBIN = 0,    // Round-robin distribution across queues
        SEQUENCE_MOD = 1,   // Based on PDCP sequence number % 4
        PORT_BASED = 2,     // Based on TCP/UDP port numbers
        DSCP_BASED = 3,     // Based on DSCP markings (if available)
        PACKET_SIZE = 4     // Based on packet size
    };

public:
    /**
     * @brief Constructor with configurable classification mode
     * @param mode Classification mode to use
     * @param numQueues Number of available queues (default: 4)
     */
    explicit QosProfiler(ClassificationMode mode = ClassificationMode::SEQUENCE_MOD,
                         uint8_t numQueues = 4);

    /**
     * @brief Destructor
     */
    ~QosProfiler();

    /**
     * @brief Classify a packet and determine target queue
     * @param packet Pointer to packet data (including PDCP header)
     * @param len Packet length
     * @return Queue ID (0-3)
     */
    uint8_t classifyPacket(const unsigned char* packet, size_t len);

    /**
     * @brief Get current classification mode
     * @return Current classification mode
     */
    ClassificationMode getMode() const { return mode_; }

    /**
     * @brief Set classification mode
     * @param mode New classification mode
     */
    void setMode(ClassificationMode mode) { mode_ = mode; }

    /**
     * @brief Get number of queues
     * @return Number of queues
     */
    uint8_t getNumQueues() const { return numQueues_; }

    /**
     * @brief Get statistics for packet classification
     * @param queueId Queue ID (0-3)
     * @return Number of packets assigned to this queue
     */
    uint64_t getQueueStats(uint8_t queueId) const;

    /**
     * @brief Reset classification statistics
     */
    void resetStats();

private:
    /**
     * @brief Classify based on round-robin
     * @return Queue ID
     */
    uint8_t classifyRoundRobin();

    /**
     * @brief Classify based on PDCP sequence number
     * @param packet Packet data with PDCP header
     * @param len Packet length
     * @return Queue ID
     */
    uint8_t classifyBySequence(const unsigned char* packet, size_t len);

    /**
     * @brief Classify based on port numbers
     * @param packet Packet data
     * @param len Packet length
     * @return Queue ID
     */
    uint8_t classifyByPort(const unsigned char* packet, size_t len);

    /**
     * @brief Classify based on packet size
     * @param len Packet length
     * @return Queue ID
     */
    uint8_t classifyBySize(size_t len);

private:
    ClassificationMode mode_;           ///< Current classification mode
    uint8_t numQueues_;                ///< Number of available queues
    uint64_t roundRobinCounter_;       ///< Counter for round-robin mode
    uint64_t queueStats_[4];          ///< Per-queue packet statistics
};

#endif // QOS_PROFILER_HPP