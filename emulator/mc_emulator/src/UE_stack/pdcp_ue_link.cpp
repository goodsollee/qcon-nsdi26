#include "pdcp_ue_link.hpp"
#include "log.hpp"
#include <chrono>

PdcpUELink::PdcpUELink(const PdcpConfig& cfg)
    : PdcpContext(PdcpRole::RECEIVER),
      logger(cfg.common.logFoldername + "/ue_processing.csv") {

    // Initialize RLC module - set to receiver mode
    rlcModule = std::make_shared<RlcReceiverModule>(cfg.ue.rlc, cfg.common.logFoldername);
    
    // Initialize MAC module
    macModule = std::make_shared<MacReceiverModule>(rlcModule, cfg.ue.mac);
    
    // Initialize PHY module
    phyModule = std::make_shared<PhyReceiverModule>();
    
    // Connect the layers
    phyModule->setMacReceiver(macModule);
    
    // Set up RLC to PDCP delivery with multi-queue support
    rlcModule->setMultiQueueDeliveryCallback([this](const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority) {
        this->onRlcToPdcpMultiQueueDelivery(packet, len, queue_id, priority);
    });

    // Fallback to legacy callback for backward compatibility
    rlcModule->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onRlcToPdcpDelivery(packet, len);
    });
    
    // Set up callback for MAC to deliver reassembled packets
    macModule->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onMacToRlcDelivery(packet, len);
    });

    // Set up Status PDU callbacks chain - RLC -> MAC -> PHY
    rlcModule->setStatusPduCallback([this](const RlcStatusPdu& status_pdu) {
        LOG_DEBUG("[PDCP UE Link] RLC Status PDU with ACK SN: " << status_pdu.ack_sn 
                 << ", forwarding to MAC");
        
        if (macModule) {
            macModule->sendStatusPdu(status_pdu);
        }
    });
    
    // MAC to PHY Status PDU callback
    macModule->setStatusPduCallback([this](const RlcStatusPdu& status_pdu) {
        LOG_DEBUG("[PDCP UE Link] MAC Status PDU with ACK SN: " << status_pdu.ack_sn 
                 << ", forwarding to PHY");
        
        if (phyModule) {
            phyModule->sendStatusPdu(status_pdu);
            
            // Log the transmission in our CSV file
            logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()) +
                ",RLC->MAC->PHY,StatusPDU_sent," + std::to_string(status_pdu.ack_sn));
        }
    });

    // Start the receiver modules
    macModule->start();
    phyModule->start();
    
    LOG_INFO("[PDCP UE Link] UE initialized with RLC/MAC/PHY/PDCP modules");
    
    // Write CSV header for logging
    logger.logLine("timestamp,layer,event,packet_size");
}

PdcpUELink::~PdcpUELink() {
    // Stop modules in reverse order
    if (macModule) macModule->stop();
    if (phyModule) phyModule->stop();
    
    LOG_INFO("[PDCP UE Link] UE modules cleaned up");
}

size_t PdcpUELink::processPacket(unsigned char* packet, size_t len) {
    // This implements the PdcpContext interface
    // For UE, we'll forward to PHY layer as that's the entry point for receiver
    return processPhyPacket(packet, len) ? len : 0;
}

bool PdcpUELink::processPhyPacket(const unsigned char* packet, size_t len) {
    if (phyModule) {
        logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) +
            ",PHY,received," + std::to_string(len));
            
        return phyModule->processReceivedPacket(packet, len);
    }
    return false;
}

void PdcpUELink::checkAndDeliverPackets() {
    if (macModule) {
        // First try to deliver any packets that have been reassembled at the MAC layer
        while (macModule->deliverReassembledPackets()) {
            // Continue delivering until no more packets available
        }
    }
}

void PdcpUELink::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
}

void PdcpUELink::setMultiQueueDeliveryCallback(std::function<void(const unsigned char*, size_t, uint8_t, uint8_t)> callback) {
    multiQueueDeliveryCallback = callback;
}

void PdcpUELink::getRlcStats(uint32_t& reassembled, uint32_t& dequeued,
                    uint32_t& dropped, uint32_t& segments_processed) const {
    if (rlcModule) {
        rlcModule->getStats(reassembled, dequeued, dropped, segments_processed);
    } else {
        reassembled = dequeued = dropped = segments_processed = 0;
    }
}

void PdcpUELink::getMacStats(uint32_t& processed_packets, uint32_t& processed_bytes) const {
    if (macModule) {
        processed_packets = macModule->getPacketsProcessed();
        processed_bytes = macModule->getBytesProcessed();
    } else {
        processed_packets = processed_bytes = 0;
    }
}

void PdcpUELink::getPhyStats(uint32_t& received_packets, uint32_t& received_bytes) const {
    if (phyModule) {
        received_packets = phyModule->getPacketsReceived();
        received_bytes = phyModule->getBytesReceived();
    } else {
        received_packets = received_bytes = 0;
    }
}

void PdcpUELink::onPhyToMacDelivery(const unsigned char* packet, size_t len) {
    // This method might not be needed since we directly set up the MAC module
    // with the PHY module in the constructor
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",PHY->MAC,delivered," + std::to_string(len));
}

void PdcpUELink::onMacToRlcDelivery(const unsigned char* packet, size_t len) {
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",MAC->RLC,delivered," + std::to_string(len));
    
    if (rlcModule) {
        rlcModule->processSegment(packet, len);
    }
}

void PdcpUELink::onRlcToPdcpDelivery(const unsigned char* packet, size_t len) {
    // Legacy callback without queue information
    onRlcToPdcpMultiQueueDelivery(packet, len, 0, 0);
}

void PdcpUELink::onRlcToPdcpMultiQueueDelivery(const unsigned char* packet, size_t len, uint8_t queue_id, uint8_t priority) {
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",RLC->PDCP,delivered," + std::to_string(len) +
        ",queue_" + std::to_string(static_cast<int>(queue_id)) +
        ",priority_" + std::to_string(static_cast<int>(priority)));

    // Prefer multi-queue callback if available
    if (multiQueueDeliveryCallback) {
        multiQueueDeliveryCallback(packet, len, queue_id, priority);
        return;
    }

    // Fallback to legacy callback
    if (deliveryCallback) {
        deliveryCallback(packet, len);
    }
}