#ifndef PDCP_SENDER_HPP
#define PDCP_SENDER_HPP

#include "pdcp_common.hpp"
#include "async_logger.h"
#include "pdcp_config.h"
#include "async_logger.h"
#include <atomic>
#include <mutex>

class PdcpSender : public PdcpContext {
public:
    PdcpSender(const PdcpConfig& cfg);
    virtual ~PdcpSender();
    
    // Process a packet (add sequence number)
    virtual size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Get current sequence number (for statistics)
    uint32_t getSequence() const;
    
private:
    std::atomic<uint32_t> currentEpoch;
    std::atomic<uint32_t> nextSequence;

    AsyncLogger async_logger;
};

#endif /* PDCP_SENDER_HPP */