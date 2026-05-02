#ifndef PDCP_CONFIG_H
#define PDCP_CONFIG_H

#include <string>
#include "pdcp_common.hpp"

// Don't redefine constants that are already in pdcp_common.hpp
// #define PDCP_MAX_PACKET_SIZE 4096
// #define PDCP_MAX_REORDER_WINDOW 1024
// #define PDCP_SEQ_LIMIT 0xFFFFFFFF
// #define PDCP_DEFAULT_REORDER_TIMEOUT_MS 100

/**
 * @brief Common configuration parameters for any PDCP role.
 */
struct PdcpCommonConfig
{
    PdcpRole    role;        ///< PDCP role (SENDER, RECEIVER, LINK, etc.)
    std::string logFoldername; ///< Name of the CSV or log file for AsyncLogger
    // Add more fields here that are shared by all roles

    // Provide a default constructor
    PdcpCommonConfig()
        : role(PdcpRole::NONE),
          logFoldername("default_log")
    {}
};

/**
 * @brief Configuration parameters specific to PDCP Receiver.
 */
struct PdcpReceiverConfig
{
    int reorderTimeoutMs; ///< Reordering timeout in milliseconds

    PdcpReceiverConfig()
        : reorderTimeoutMs(300)  // Default
    {}
};

/**
 * @brief Configuration parameters specific to PDCP Link.
 */
struct PdcpLinkConfig
{
    int linkId;                 ///< Link identifier
    
    // RLC parameters
    uint32_t bufferSize;        ///< RLC buffer size in packets
    
    // MAC parameters
    double slotDuration;      ///< MAC slot duration in milliseconds
    
    // PHY parameters
    uint32_t bwUpdateInterval;  ///< Bandwidth update interval in milliseconds
    std::string bandwidthTraceFile; ///< Path to bandwidth trace file (empty for static bandwidth)
    uint32_t fixedBandwidth;    ///< Fixed bandwidth in bits per second (used when no trace file)

    PdcpLinkConfig()
        : linkId(0),
          bufferSize(100),      // Default 100 packets
          slotDuration(1),      // Default 1 millisecond
          bwUpdateInterval(100),// Default 100 milliseconds
          bandwidthTraceFile(""),
          fixedBandwidth(5000000) // Default 5 Mbps
    {}
};

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
    PdcpReceiverConfig receiver; ///< Receiver-specific fields
    PdcpLinkConfig     link;     ///< Link-specific fields
    PdcpSenderConfig   sender;   ///< Sender-specific fields

    // Provide a default constructor that sets all defaults
    PdcpConfig() {}
};

#endif // PDCP_CONFIG_H