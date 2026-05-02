#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <zmq.hpp>
#include <jsoncpp/json/json.h>
#include "rtp_profiler/packet_sniffer.h"
#include "multi_conn_agent/control_interface.h"
#include "../logger/Logger.h"

using namespace multiconn;

class RANIntelligentController : public std::enable_shared_from_this<RANIntelligentController> {
public:
    // Factory method to create instance
    static std::shared_ptr<RANIntelligentController> create(const std::string& zmq_address);
    
    ~RANIntelligentController();
    
    // Initialize after construction
    bool initialize();
    
    void run();
    void stop();
    
    void onNewPacket(const Packet& packet);
    void onNewFrame(const FrameInfo& frameInfo, bool is_complete);
    void sendControlMessageToRAN(const RICControlMessage& msg);
    void getPdcpPacket(const char* packet_data, size_t packet_len, const struct timeval& timestamp);

private:
    // Private constructor to force usage of factory method
    explicit RANIntelligentController(const std::string& zmq_address);
    
    // Initialization helper functions
    bool initializeZMQ();
    bool initializePacketSniffer();
    bool initializeControlInterface();
    
    // Helper functions
    void send_control_info();
    void receive_ran_info();
    void process_ran_info(const std::string& info);
    void update_control_parameters();
    void sendFrameInfoToRAN(const FrameInfo& frameInfo);

    // Member variables
    std::string zmq_address_;
    zmq::context_t context;
    zmq::socket_t socket_receive;
    zmq::socket_t socket_send;
    
    // Control parameters
    bool multipath_routing_active;
    double retransmission_probability_dl_lte;
    double retransmission_probability_ul_lte;
    double retransmission_probability_dl_nr;
    double retransmission_probability_ul_nr;
    int preallocated_tbs_lte;
    int preallocated_tbs_nr;

    std::unique_ptr<PacketCaptureModule> packet_sniffer_;
    std::unique_ptr<ControlInterface> control_interface_;
    std::atomic<bool> running;
};