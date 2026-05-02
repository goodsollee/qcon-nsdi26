#include "UE.hpp"
#include "log.h"

UE::UE(const UEConfig& cfg)
    : logger(cfg.logFolder + "/ue_processing.csv") {
    
    LOG_INFO("[UE] Initializing UE modules");
    
    // Create PDCP config
    PdcpConfig pdcpCfg;
    pdcpCfg.common.logFoldername = cfg.logFolder;
    pdcpCfg.common.role = PdcpRole::RECEIVER;
    pdcpCfg.receiver.reorderTimeoutMs = cfg.pdcpReorderTimeoutMs;
    
    // Initialize RLC module - set to receiver mode
    rlcModule = std::make_shared<RlcReceiverModule>(
        cfg.rlcBufferSize, cfg.rlcReassemblyTimeoutMs);
    
    // Initialize MAC module
    macModule = std::make_shared<MacReceiverModule>(rlcModule);
    
    // Initialize PHY module
    phyModule = std::make_shared<PhyReceiverModule>();
    
    // Connect the layers
    phyModule->setMacReceiver(macModule);
    
    // Set up RLC to PDCP delivery
    rlcModule->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onRlcToPdcpDelivery(packet, len);
    });
    
    // Set up MAC to RLC delivery is handled internally by Mac module constructor
    // which takes a reference to the RLC module
    
    // Set up PHY to MAC delivery is handled by connecting the modules above
    
    // Set up callback for MAC to deliver reassembled packets
    macModule->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onMacToRlcDelivery(packet, len);
    });
    
    // Start the receiver modules
    macModule->start();
    phyModule->start();
    
    LOG_INFO("[UE] UE initialized with RLC/MAC/PHY/PDCP modules");
    
    // Write CSV header for logging
    logger.logLine("timestamp,layer,event,packet_size");
}

UE::~UE() {
    // Stop modules in reverse order
    if (macModule) macModule->stop();
    if (phyModule) phyModule->stop();
    
    LOG_INFO("[UE] UE modules cleaned up");
}

bool UE::processPhyPacket(const unsigned char* packet, size_t len) {
    if (phyModule) {
        logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) +
            ",PHY,received," + std::to_string(len));
            
        return phyModule->processReceivedPacket(packet, len);
    }
    return false;
}

void UE::checkAndDeliverPackets() {
    if (macModule) {
        // First try to deliver any packets that have been reassembled at the MAC layer
        while (macModule->deliverReassembledPackets()) {
            // Continue delivering until no more packets available
        }
    }
}

void UE::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
}

void UE::getPdcpStats(uint32_t& nextExpected, uint32_t& highestReceived,
                     uint32_t& delivered, uint32_t& dropped) const {
    if (pdcpModule) {
        pdcpModule->getStats(nextExpected, highestReceived, delivered, dropped);
    } else {
        nextExpected = highestReceived = delivered = dropped = 0;
    }
}

void UE::getRlcStats(uint32_t& reassembled, uint32_t& dequeued,
                    uint32_t& dropped, uint32_t& segments_processed) const {
    if (rlcModule) {
        rlcModule->getStats(reassembled, dequeued, dropped, segments_processed);
    } else {
        reassembled = dequeued = dropped = segments_processed = 0;
    }
}

void UE::getMacStats(uint32_t& processed_packets, uint32_t& processed_bytes) const {
    if (macModule) {
        processed_packets = macModule->getPacketsProcessed();
        processed_bytes = macModule->getBytesProcessed();
    } else {
        processed_packets = processed_bytes = 0;
    }
}

void UE::getPhyStats(uint32_t& received_packets, uint32_t& received_bytes) const {
    if (phyModule) {
        received_packets = phyModule->getPacketsReceived();
        received_bytes = phyModule->getBytesReceived();
    } else {
        received_packets = received_bytes = 0;
    }
}

void UE::onPhyToMacDelivery(const unsigned char* packet, size_t len) {
    // This method might not be needed since we directly set up the MAC module
    // with the PHY module in the constructor
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",PHY->MAC,delivered," + std::to_string(len));
}

void UE::onMacToRlcDelivery(const unsigned char* packet, size_t len) {
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",MAC->RLC,delivered," + std::to_string(len));
    
    if (rlcModule) {
        rlcModule->processSegment(packet, len);
    }
}

void UE::onRlcToPdcpDelivery(const unsigned char* packet, size_t len) {
    logger.logLine(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) +
        ",RLC->PDCP,delivered," + std::to_string(len));
    
    if (deliveryCallback) {
        deliveryCallback(packet, len);
    }
}
