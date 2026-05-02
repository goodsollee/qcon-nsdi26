#ifndef PHY_RECEIVER_HPP
#define PHY_RECEIVER_HPP

#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <string>
#include <memory>
#include "log.hpp"
#include "mac_receiver.hpp" // Include MAC receiver

// PHY Receiver Module - handles reception of segmented packets
class PhyReceiverModule {
public:
    PhyReceiverModule();
    ~PhyReceiverModule();
    
    // Start the receiver module
    void start();
    
    // Stop the receiver module
    void stop();
    
    // Process received packet - forward to MAC layer
    bool processReceivedPacket(const unsigned char* packet, size_t len);
    
    // Set MAC layer
    void setMacReceiver(std::shared_ptr<MacReceiverModule> mac);
    
    // Set callback for delivering packets to the next hop
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Get statistics
    uint32_t getBytesReceived() const;
    uint32_t getPacketsReceived() const;


    // Add this new method to set the status PDU callback
    void setStatusPduCallback(std::function<void(const RlcStatusPdu&)> callback) {
        statusPduCallback = callback;
        LOG_INFO("[MAC Receiver] Status PDU callback set");
    }

    // Add this method to handle status PDUs from MAC
    bool sendStatusPdu(const RlcStatusPdu& status_pdu) {
        
        if (!running.load()) {
            LOG_WARN("[PHY Receiver] Attempted to send Status PDU while not running");
            return false;
        }
        
        LOG_DEBUG("[PHY Receiver] Forwarding Status PDU with ACK SN: " << status_pdu.ack_sn);
        
        if (statusPduCallback) {
            statusPduCallback(status_pdu);
            return true;
        }
        
        LOG_WARN("[PHY Receiver] No Status PDU callback set, cannot forward Status PDU");
        return false;
    }

private:
    std::shared_ptr<MacReceiverModule> macReceiver;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    std::atomic<uint32_t> bytesReceived;
    std::atomic<uint32_t> packetsReceived;
    std::atomic<bool> running;
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;

    std::function<void(const RlcStatusPdu&)> statusPduCallback;
};

#endif // PHY_RECEIVER_HPP