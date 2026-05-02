#ifndef PDCP_CONFIG_H
#define PDCP_CONFIG_H

#include <string>
#include <vector>
#include "pdcp_common.hpp"

// Maximum reordering window size
#define PDCP_MAX_REORDER_WINDOW 1024*8
// Default reordering timeout in milliseconds
#define PDCP_DEFAULT_REORDER_TIMEOUT_MS 300

/**
 * @brief Common configuration parameters for any PDCP role.
 */
struct PdcpCommonConfig
{
    PdcpRole    role;        ///< PDCP role (SENDER, RECEIVER, LINK, etc.)
    std::string logFoldername; ///< Name of the CSV or log file for AsyncLogger
    uint8_t path_num;   ///< Number of paths for KPM reporting
    std::string log_level = "INFO";
    // Add more fields here that are shared by all roles

    // Provide a default constructor
    PdcpCommonConfig()
        : role(PdcpRole::NONE),
          logFoldername("default_log")
    {}
};

// RIC interface configuration
struct PdcpRicConfig {
    bool ric_enabled;
    std::string ipAddress; // IP address of the RIC
    int kpmPort = 5555;    // KPM port on the RIC
    int rcPort = 5556;     // RC port on the RIC
    int localKpmPort = 6555; // Local KPM port
    int localRcPort = 6556;  // Local RC port
    uint32_t kpm_report_interval_ms;
};


struct RlcReceiverConfig
{
    uint8_t max_retransmissions; ///< Maximum number of retransmissions
    uint32_t bufferSize; ///< RLC buffer size in packets
    uint32_t reassembly_timeout_ms; ///< RLC reassembly timeout in milliseconds
    uint32_t status_pdu_interval_ms; ///< Status PDU interval in milliseconds
    uint32_t t_statusProhibit_ms; ///< Status PDU prohibit timer in milliseconds
};

struct MacReceiverConfig
{
    uint32_t txOpportunityInterval; 
    uint32_t txDelay;
};

struct RlcQueueConfig
{
    uint8_t  queueId;                 ///< Identifier for this queue
    uint8_t  priority;                ///< Scheduling priority (higher value -> higher priority)
    uint32_t bufferSize;              ///< Buffer size in packets
    uint32_t max_retransmissions;     ///< Maximum retransmissions for this queue
    uint32_t retransmission_timeout_ms; ///< Retransmission timeout in milliseconds
    uint32_t poll_retransmit_timer_ms;  ///< Poll retransmit timer in milliseconds

    RlcQueueConfig()
        : queueId(0),
          priority(0),
          bufferSize(0),
          max_retransmissions(0),
          retransmission_timeout_ms(0),
          poll_retransmit_timer_ms(0)
    {}
};

// For backward compatibility with existing code
typedef struct PdcpDUConfig {
    int linkId;
    uint32_t bufferSize;
    uint32_t slotDuration;
    std::string bandwidthTraceFile;
    uint32_t bwUpdateInterval;
    uint32_t fixedBandwidth;
    uint32_t max_retransmissions;
    uint32_t reassembly_timeout_ms;
    uint32_t status_prohibit_ms;
    uint32_t poll_retransmit_timer;
    uint32_t retransmission_timeout_ms;

    std::vector<RlcQueueConfig> queues; ///< Per-queue configuration

    // Constructor to enable conversion from PdcpDULinkConfig
    PdcpDUConfig() {}
} PdcpDUConfig;


// Configuration structure for UE Link
struct PdcpUEConfig {
    // Basic configuration
    std::string logFolder;

    RlcReceiverConfig rlc;
    MacReceiverConfig mac;
    std::vector<RlcQueueConfig> queues; ///< Per-queue configuration for UE side

    // Default valUEs
    PdcpUEConfig() {}
};

// For backward compatibility with existing code
typedef struct PdcpReceiverConfig {
    uint32_t reordering_timeout_ms; ///< PDCP reordering timeout in milliseconds
    
    // Constructor to enable conversion from PdcpDULinkConfig
    PdcpReceiverConfig() {}
} PdcpReceiverConfig;


/**
 * @brief Configuration parameters specific to PDCP Sender.
 */
struct PdcpSenderConfig
{
    int split_policy;

    PdcpSenderConfig()
        : split_policy(0)
    {}
};

/**
 * @brief Top-level PDCP configuration, containing a common config
 *        plus a role-specific config.
 *
 * You can extend or modify these structs as needed.
 */
struct PdcpConfig
{
    PdcpCommonConfig   common;   ///< Common config fields
    PdcpSenderConfig   sender;   ///< Sender-specific fields
    PdcpUEConfig     ue;       ///< UE-specific fields
    PdcpDUConfig     du;       ///< UE-specific fields
    PdcpReceiverConfig receiver; ///< Receiver-specific fields
    PdcpRicConfig ric;

    // Provide a default constructor that sets all defaults
    PdcpConfig() {}
};

#endif // PDCP_CONFIG_HPP
