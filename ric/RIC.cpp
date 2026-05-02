// RANIntelligentController.cpp
#include "RIC.h"
#include <iostream>
#include <thread>
#include <chrono>
#include "utils/base64.h" 

static const char* RIC_MODULE_NAME = "ric";

std::shared_ptr<RANIntelligentController> RANIntelligentController::create(const std::string& zmq_address) {
    // Create a shared_ptr with a custom deleter to ensure proper cleanup
    struct make_shared_enabler : public RANIntelligentController {
        explicit make_shared_enabler(const std::string& addr) : RANIntelligentController(addr) {}
    };
    return std::make_shared<make_shared_enabler>(zmq_address);
}

RANIntelligentController::RANIntelligentController(const std::string& zmq_address)
    : zmq_address_(zmq_address)
    , context(1)
    , socket_receive(context, zmq::socket_type::pull)
    , socket_send(context, zmq::socket_type::push)
    , multipath_routing_active(false)
    , retransmission_probability_dl_lte(0.1)
    , retransmission_probability_ul_lte(0.1)
    , retransmission_probability_dl_nr(0.1)
    , retransmission_probability_ul_nr(0.1)
    , preallocated_tbs_lte(10)
    , preallocated_tbs_nr(10)
    , running(true) {
    
    // Initialize logging
    Logger::getInstance().setLogFile("ric.log");
    LOG_INFO(RIC_MODULE_NAME, "RIC starting up...");
}

bool RANIntelligentController::initialize() {
    if (!initializeZMQ()) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize ZMQ");
        return false;
    }
    
    if (!initializePacketSniffer()) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize packet sniffer");
        return false;
    }
    
    if (!initializeControlInterface()) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize control interface");
        return false;
    }
    
    LOG_INFO(RIC_MODULE_NAME, "RIC initialization complete");
    return true;
}

bool RANIntelligentController::initializeZMQ() {
    try {
        // Use stored zmq_address_ for socket connection
        socket_send.connect("tcp://localhost:5557");
        LOG_INFO(RIC_MODULE_NAME, "Connected send socket to port 5557");

        socket_receive.bind(zmq_address_);  // Use the stored address
        LOG_INFO(RIC_MODULE_NAME, "Listening for messages on ", zmq_address_);
        return true;
    } catch (const zmq::error_t& e) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize ZMQ: ", e.what());
        return false;
    }
}

bool RANIntelligentController::initializePacketSniffer() {
    LOG_INFO(RIC_MODULE_NAME, "Initializing packet sniffer...");
    try {
        // Create a weak_ptr to avoid cyclic reference
        std::weak_ptr<RANIntelligentController> weak_self = weak_from_this();
        
        auto packetCallback = [weak_self](const Packet& packet) {
            if (auto self = weak_self.lock()) {
                self->onNewPacket(packet);
            }
        };
        
        auto frameCallback = [weak_self](const FrameInfo& frameInfo, bool is_completed) {
            if (auto self = weak_self.lock()) {
                self->onNewFrame(frameInfo, is_completed);
            }
        };
        
        std::shared_ptr<std::atomic<bool>> sniff_running = 
            std::make_shared<std::atomic<bool>>(true);
        
        packet_sniffer_ = std::make_unique<PacketCaptureModule>(
            sniff_running, packetCallback, frameCallback);
            
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize packet sniffer: ", e.what());
        return false;
    }
}

bool RANIntelligentController::initializeControlInterface() {
    LOG_INFO(RIC_MODULE_NAME, "Initializing Control Interface...");
    try {
        // Create a weak_ptr to avoid cyclic reference
        std::weak_ptr<RANIntelligentController> weak_self = weak_from_this();
        
        auto link_allocation_cb = [weak_self](const RICControlMessage& msg) {
            if (auto self = weak_self.lock()) {
                self->sendControlMessageToRAN(msg);
            }
        };
        
        auto packet_forward_cb = [weak_self](const RICControlMessage& msg) {
            if (auto self = weak_self.lock()) {
                self->sendControlMessageToRAN(msg);
            }
        };

        control_interface_ = std::make_unique<multiconn::ControlInterface>(
            link_allocation_cb,
            packet_forward_cb
        );
        
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to initialize control interface: ", e.what());
        return false;
    }
}

RANIntelligentController::~RANIntelligentController() {
    stop();
}

void RANIntelligentController::run() {
    LOG_INFO(RIC_MODULE_NAME, "Run RIC..");
    packet_sniffer_->start();

    while (running.load()) {
        try {
            receive_ran_info();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (const std::exception& e) {
            LOG_ERROR(RIC_MODULE_NAME, "Error in control loop: ", e.what());
        }
    }
}

void RANIntelligentController::stop() {
    LOG_INFO(RIC_MODULE_NAME, "Stopping RIC...");
    running = false;
    LOG_INFO(RIC_MODULE_NAME, "RIC stopped");
}

void RANIntelligentController::receive_ran_info() {
    zmq::message_t message;
    try {
        auto result = socket_receive.recv(message, zmq::recv_flags::none | zmq::recv_flags::dontwait);
        if (result) {
            std::string ran_info(static_cast<char*>(message.data()), message.size());
            LOG_DEBUG(RIC_MODULE_NAME, "Received RAN message, size: ", message.size(), " bytes");
            process_ran_info(ran_info);
        }
    } catch (const zmq::error_t& e) {
        LOG_ERROR(RIC_MODULE_NAME, "ZMQ receive error: ", e.what());
    }
}

void RANIntelligentController::process_ran_info(const std::string& info) {
    Json::Value msg;
    Json::Reader reader;
    
    if (reader.parse(info, msg)) {
        if (msg.isMember("header") && msg.isMember("payload") && 
            msg.isMember("ue_id") && msg.isMember("sn") && 
            msg.isMember("timestamp")) {
            
            std::string header = msg["header"].asString();
            std::string payload = msg["payload"].asString();
            int ue_id = msg["ue_id"].asInt();
            int link_id = msg["link_id"].asInt();
            int sn = msg["sn"].asInt();
            uint64_t timestamp = msg["timestamp"].asUInt64();
            
            LOG_DEBUG(RIC_MODULE_NAME, "Processing message - Header: ", header,
                     ", UE: ", ue_id,
                     ", SN: ", sn,
                     ", Size: ", payload.size());

            struct timeval tv;
            tv.tv_sec = timestamp / 1000000000LL;
            tv.tv_usec = (timestamp % 1000000000LL) / 1000;
            
            // msg from PDCP layer
            if (header == "pdcp") {
                LOG_DEBUG(RIC_MODULE_NAME, "Processing PDCP packet for UE ", ue_id);
                packet_sniffer_->processPdcpPacket(payload.c_str(), 
                                               payload.size(), 
                                               ue_id, sn, tv);
            }
            // msg from RLC layer
            else if (header == "rlc") {
                LOG_DEBUG(RIC_MODULE_NAME, "Processing RLC sequence number for UE ", ue_id);

                // Create and process single RLCInfo
                RLCInfo rlc_info;
                rlc_info.ue_id = ue_id;
                rlc_info.link_id = link_id;
                rlc_info.pdcp_sn = sn;
                rlc_info.rlc_sn = std::stoi(payload);  // Single RLC SN
                rlc_info.queue_size = msg["queue size"].asInt();
                rlc_info.is_ack = false;
                rlc_info.timestamp = timestamp;

                LOG_DEBUG(RIC_MODULE_NAME, "Created RLCInfo object for UE ", rlc_info.ue_id, 
                        " Link id: ", rlc_info.link_id,
                        " PDCP SN: ", rlc_info.pdcp_sn,
                        " RLC SN: ", rlc_info.rlc_sn);

                // Process single RLCInfo
                control_interface_->process_rlc_update(rlc_info);
            }
            else if (header == "delay_info") {
                // Decrypt the payload
                const char* payload_str = payload.c_str();

                // Parse the comma-separated values
                uint64_t drx_wake_time = 0;
                uint32_t xn_delay_ms = 0;

                std::string payload_copy(payload_str); // Create a std::string for manipulation
                size_t comma_pos = payload_copy.find(',');
                if (comma_pos != std::string::npos) {
                    try {
                        // Extract DRX wake time
                        drx_wake_time = std::stoull(payload_copy.substr(0, comma_pos));
                        // Extract Xn delay (convert after the comma)
                        xn_delay_ms = std::stoul(payload_copy.substr(comma_pos + 1));
                    } catch (const std::exception& e) {
                        LOG_ERROR(RIC_MODULE_NAME, "Failed to parse delay_info payload: ", payload_copy,
                                ", error: ", e.what());
                        return;
                    }
                } else {
                    LOG_ERROR(RIC_MODULE_NAME, "Invalid format for delay_info payload: ", payload_copy);
                    return;
                }

                // Create DelayInfo structure
                DelayInfo delay_info;
                delay_info.ue_id = ue_id;                 // Extracted from msg["ue_id"]
                delay_info.link_id = link_id;             // Example, set appropriate link_id
                delay_info.drx_wake_time = drx_wake_time; // Extracted from decrypted payload
                delay_info.xn_delay_ms = xn_delay_ms;     // Extracted from decrypted payload
                delay_info.timestamp = timestamp;         // Extracted from msg["timestamp"]

                LOG_DEBUG(RIC_MODULE_NAME, "Created DelayInfo object for UE ", delay_info.ue_id,
                        ", Xn delay: ", delay_info.xn_delay_ms, " ms");

                // Pass DelayInfo to control interface
                control_interface_->process_delay_update(delay_info);
            }
            // ACK from RLC layer
            else if (header == "rlc_ack") {
                LOG_DEBUG(RIC_MODULE_NAME, "Processing RLC ACK for UE ", ue_id);

                // Create and process single RLCInfo for ACK
                RLCInfo rlc_info;
                rlc_info.ue_id = ue_id;
                rlc_info.link_id = link_id;
                rlc_info.rlc_sn = sn;  // Single RLC SN
                rlc_info.acked_bytes = msg["acked bytes"].asInt();
                rlc_info.queue_size = msg["queue size"].asInt();
                rlc_info.is_ack = true;
                rlc_info.timestamp = timestamp;

                LOG_DEBUG(RIC_MODULE_NAME, "Created RLCInfo ACK for UE ", rlc_info.ue_id, 
                        " Link id: ", rlc_info.link_id,
                        " RLC SN: ", rlc_info.rlc_sn);

                // Process single ACK
                control_interface_->process_rlc_ack(rlc_info);
            }
        } else {
            if (!msg.isMember("header")) LOG_WARNING(RIC_MODULE_NAME, "Missing 'header' field");
            if (!msg.isMember("payload")) LOG_WARNING(RIC_MODULE_NAME, "Missing 'payload' field");
            if (!msg.isMember("ue_id")) LOG_WARNING(RIC_MODULE_NAME, "Missing 'ue_id' field");
            if (!msg.isMember("sn")) LOG_WARNING(RIC_MODULE_NAME, "Missing 'sn' field");
            if (!msg.isMember("timestamp")) LOG_WARNING(RIC_MODULE_NAME, "Missing 'timestamp' field");
            LOG_WARNING(RIC_MODULE_NAME, "Invalid message format: missing required fields");
            LOG_DEBUG(RIC_MODULE_NAME, "Received message: ", info);
        }
    } else {
        LOG_ERROR(RIC_MODULE_NAME, "Failed to parse JSON message: ", info);
    }
}

void RANIntelligentController::update_control_parameters() {
    LOG_DEBUG(RIC_MODULE_NAME, "Updating control parameters");
}

// Add this helper function to convert Packet to PDCPPacketInfo
PDCPPacketInfo convertToPDCPPacketInfo(const Packet& packet) {
    PDCPPacketInfo pdcp_info;
    
    // Copy basic fields
    pdcp_info.ue_id = packet.ue_id;
    pdcp_info.pdcp_sn = packet.pdcp_sn;
    pdcp_info.data = packet.data;  // Copy the vector
    
    // Convert timespec to timeval
    pdcp_info.timestamp.tv_sec = packet.ts.tv_sec;
    pdcp_info.timestamp.tv_usec = packet.ts.tv_nsec / 1000;
    
    // Set frame-related information from RTPPacketInfo
    pdcp_info.frame_id = packet.packet_info.frame_id;
    pdcp_info.is_marker = packet.packet_info.marker;
    
    // Initialize empty RLC info map
    pdcp_info.rlc_info.clear();
    
    return pdcp_info;
}

// Then modify your onNewPacket function
void RANIntelligentController::onNewPacket(const Packet& packet) {
    LOG_DEBUG(RIC_MODULE_NAME, "New packet received - UE: ", packet.ue_id,
              ", SN: ", packet.pdcp_sn,
              ", Size: ", packet.data.size());

    // Convert Packet to PDCPPacketInfo
    PDCPPacketInfo pdcp_info = convertToPDCPPacketInfo(packet);

    // Create a vector for the single packet
    //std::vector<PDCPPacketInfo> packets = {pdcp_info};

    // Forward to control interface
    control_interface_->process_pdcp_packet(pdcp_info);
    
    LOG_DEBUG(RIC_MODULE_NAME, "Forwarded PDCP packet - Frame: ", pdcp_info.frame_id,
              ", Marker: ", pdcp_info.is_marker);
}

void RANIntelligentController::onNewFrame(const FrameInfo& frameInfo, bool is_completed) {
    LOG_DEBUG(RIC_MODULE_NAME, "New frame event - UE: ", frameInfo.ue_id,
              ", Size: ", frameInfo.size,
              ", Frame rate: ", frameInfo.frame_rate,
              ", Completed: ", is_completed);

    // Forward to control interface
    control_interface_->handle_frame_arrival(frameInfo.ue_id, frameInfo.frame_id, frameInfo.size, is_completed);
}

void RANIntelligentController::sendControlMessageToRAN(const RICControlMessage& msg) {
    try {
        // Serialize control message to JSON
        Json::Value json_msg;
        json_msg["header"] = (msg.message_type == RICMessageType::LINK_ALLOCATION) ? 
                            "link_allocation" : "pdcp_forward";
        json_msg["ue_id"] = msg.ue_id;
        json_msg["frame_id"] = msg.frame_id;
        json_msg["timestamp"] = Json::UInt64(msg.timestamp);

        if (msg.message_type == RICMessageType::LINK_ALLOCATION) {
            Json::Value links(Json::arrayValue);
            for (const auto& link : msg.link_controls) {
                Json::Value link_info;
                link_info["link_id"] = link.link_id;
                link_info["ratio"] = link.allocation_ratio;
                links.append(link_info);
            }
            json_msg["links"] = links;
        } else {
            // Handle PDCP packets
            Json::Value packets(Json::arrayValue);
            for (const auto& pkt : msg.pdcp_packets) {
                Json::Value packet_info;
                packet_info["pdcp_sn"] = pkt.pdcp_sn;
                packet_info["data"] = utils::base64_encode(pkt.data);
                packets.append(packet_info);
            }
            json_msg["packets"] = packets;
        }

        // Send JSON message through ZMQ
        Json::StreamWriterBuilder writer;
        std::string message_str = Json::writeString(writer, json_msg);
        
        zmq::message_t zmq_msg(message_str.size());
        memcpy(zmq_msg.data(), message_str.c_str(), message_str.size());
        socket_send.send(zmq_msg, zmq::send_flags::none);

        LOG_DEBUG(RIC_MODULE_NAME, "Sent control message to RAN - Type: ", 
                 (msg.message_type == RICMessageType::LINK_ALLOCATION) ? 
                 "link_allocation" : "pdcp_forward");
    } catch (const std::exception& e) {
        LOG_ERROR(RIC_MODULE_NAME, "Error sending control message to RAN: ", e.what());
    }
}

void RANIntelligentController::sendFrameInfoToRAN(const FrameInfo& frameInfo) {
    try {
        Json::Value message;
        message["header"] = "frame_info";
        message["frame_size"] = static_cast<Json::UInt64>(frameInfo.size);
        message["frame_rate"] = static_cast<Json::UInt64>(frameInfo.frame_rate);
        
        Json::StreamWriterBuilder writer;
        std::string message_str = Json::writeString(writer, message);
        
        zmq::message_t zmq_msg(message_str.size());
        memcpy(zmq_msg.data(), message_str.c_str(), message_str.size());
        socket_send.send(zmq_msg, zmq::send_flags::none);

        LOG_DEBUG(RIC_MODULE_NAME, "Sent frame info to RAN - Size: ", frameInfo.size,
                 ", Frame rate: ", frameInfo.frame_rate);
    } catch (const std::exception& e) {
        LOG_ERROR(RIC_MODULE_NAME, "Error sending frame info to RAN: ", e.what());
    }
}