#include "pdcp_link.hpp"
#include "log.h"
#include <iostream>

PdcpLink::PdcpLink(const PdcpConfig& cfg) 
    : PdcpContext(PdcpRole::LINK), 
      linkId(cfg.link.linkId), 
      packetsForwarded(0) {
    
    // Initialize RLC/MAC/PHY modules with segmentation support
    rlc = std::make_shared<RlcSenderModule>(cfg.link.bufferSize ? cfg.link.bufferSize : 100);
    
    mac = std::make_shared<MacSenderModule>(rlc, cfg.link.slotDuration ? cfg.link.slotDuration : 1);
    
    // Create PHY module - check if we're using trace file or static bandwidth
    if (!cfg.link.bandwidthTraceFile.empty()) {
        // Dynamic bandwidth mode with trace file
        phy = std::make_shared<PhySenderModule>(cfg.link.bandwidthTraceFile, 
                                           cfg.link.bwUpdateInterval ? cfg.link.bwUpdateInterval : 100);
        LOG_INFO("[PDCP Link " << linkId << "] Using dynamic bandwidth from trace file: " 
                  << cfg.link.bandwidthTraceFile);
    } else {
        // Static bandwidth mode
        phy = std::make_shared<PhySenderModule>("", cfg.link.bwUpdateInterval ? cfg.link.bwUpdateInterval : 100);
        phy->setBandwidth(cfg.link.fixedBandwidth);
        LOG_INFO("[PDCP Link " << linkId << "] Using static bandwidth: " 
                  << (cfg.link.fixedBandwidth / 1000) << " kbps");
    }
    
    // Set up callbacks
    mac->setPhyCallback([this](const unsigned char* packet, size_t len) -> bool {
        if (this->phy) {
            return this->phy->processPacket(packet, len);
        }
        return false;
    });
    
    mac->setGetAvailableBytesCallback([this](double slotDuration) -> size_t {
        if (this->phy) {
            return this->phy->getAvailableBytes(slotDuration);
        }
        return 0;
    });
    
    phy->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onPhyDelivery(packet, len);
    });
    
    // Start MAC and PHY threads
    mac->start();
    phy->start();
    
    LOG_INFO("[PDCP Link " << linkId << "] Initialized with RLC/MAC/PHY modules and segmentation support");
}

PdcpLink::~PdcpLink() {
    if (mac) mac->stop();
    if (phy) phy->stop();
    
    LOG_INFO("[PDCP Link " << linkId << "] Cleaned up, forwarded " 
              << packetsForwarded.load() << " packets");
}

size_t PdcpLink::processPacket(unsigned char* packet, size_t len) {
    // Forward packet to RLC layer
    bool enqueued = rlc->enqueuePacket(packet, len);
    
    if (!enqueued) {
        LOG_WARN("[PDCP Link " << linkId << "] Warning: Packet dropped due to buffer overflow");
        return 0; // Indicate packet drop
    }
    
    // We do not send packet immediately. Instead, MAC layer will pick it up
    return 0;
}

void PdcpLink::onPhyDelivery(const unsigned char* packet, size_t len) {
    // This is called when PHY layer has successfully "transmitted" the packet
    uint32_t forwardedCount = packetsForwarded.fetch_add(1, std::memory_order_relaxed) + 1;
    
    // Debug output for every 1000 packets
    if (forwardedCount % 1000 == 0) {
        LOG_INFO("[PDCP Link " << linkId << "] Forwarded " << forwardedCount 
                  << " packets, current bandwidth: " << (phy->getCurrentBandwidth() / 1000.0) 
                  << " kbps, buffer occupancy: " << rlc->getBufferOccupancy() 
                  << "/" << rlc->getBufferCapacity()
                  << ", segment buffer: " << rlc->getSegmentBufferOccupancy());
    }
}

uint32_t PdcpLink::getPacketsForwarded() const {
    return packetsForwarded.load(std::memory_order_relaxed);
}

double PdcpLink::getThroughput() const {
    if (mac) {
        return mac->getThroughput();
    }
    return 0.0;
}

double PdcpLink::getLatency() const {
    if (mac) {
        return mac->getAverageLatency();
    }
    return 0.0;
}

uint32_t PdcpLink::getCurrentBandwidth() const {
    if (phy) {
        return phy->getCurrentBandwidth();
    }
    return 0;
}

size_t PdcpLink::getRlcBufferOccupancy() const {
    if (rlc) {
        return rlc->getBufferOccupancy();
    }
    return 0;
}

void PdcpLink::setBandwidth(uint32_t bps) {
    if (phy) {
        phy->setBandwidth(bps);
    }
}

void PdcpLink::setBufferSize(uint32_t size) {
    // Note: This would normally require more complex implementation
    // as changing buffer size dynamically requires careful handling
    LOG_WARN("[PDCP Link " << linkId << "] Warning: Dynamic buffer size change not implemented");
}

void PdcpLink::setSlotDuration(uint32_t ms) {
    // Note: This would normally require restarting the MAC module
    LOG_WARN("[PDCP Link " << linkId << "] Warning: Dynamic slot duration change not implemented");
}

void PdcpLink::setBandwidthTraceFile(const std::string& filename) {
    // Note: This would normally require restarting the PHY module
    LOG_WARN("[PDCP Link " << linkId << "] Warning: Dynamic bandwidth trace file change not implemented");
}