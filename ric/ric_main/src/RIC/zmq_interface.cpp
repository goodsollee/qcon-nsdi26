// src/RIC/zmq_interface.cpp
#include "RIC/zmq_interface.hpp"
#include <chrono>

using namespace std;
const string ZMQ_MODULE = "ZMQ_INTERFACE";

namespace ric {

ZmqInterface::ZmqInterface() : context_(1), running_(false) {
    LOG_MODULE_DEBUG(ZMQ_MODULE, "ZmqInterface constructor");
}

ZmqInterface::~ZmqInterface() {
    LOG_MODULE_DEBUG(ZMQ_MODULE, "ZmqInterface destructor");
    stopListening();
    
    // Clean up sockets
    kpm_sockets_.clear();
}

bool ZmqInterface::initialize(const std::string& ric_ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    ric_ip_ = ric_ip;
    LOG_MODULE_INFO(ZMQ_MODULE, "RIC initialized with IP: " << ric_ip_);
    return true;
}

bool ZmqInterface::registerKpmProcessor(std::shared_ptr<KpmProcessor> kpm_processor)
{
    // Just store the pointer (move it in)
    kpm_processor_ = std::move(kpm_processor);
    return true;
}

bool ZmqInterface::registerRanComponent(const RanComponentInfo& component_info) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Store component information
    ran_components_[component_info.component_id] = component_info;
    
    LOG_MODULE_INFO(ZMQ_MODULE, "Registered " << component_info.component_type 
              << " with ID " << component_info.component_id
              << " at " << component_info.ip_address 
              << " (KPM: " << component_info.kpm_port 
              << ", RC: " << component_info.rc_port << ")");
    
    return true;
}

bool ZmqInterface::connectToRanComponents() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& [id, info] : ran_components_) {
        try {
            // Create KPM SUB socket for receiving metrics
            auto kpm_socket = createSocket(SocketType::SUB);
            std::string kpm_endpoint = "tcp://" + info.ip_address + ":" + std::to_string(info.kpm_port);
            kpm_socket->connect(kpm_endpoint);
            kpm_socket->set(zmq::sockopt::subscribe, id); // Subscribe to this component's ID
            kpm_sockets_[id] = std::move(kpm_socket);
            
            // Create RC REQ socket for sending commands
            std::string rc_endpoint = "tcp://" + info.ip_address + ":" + std::to_string(info.rc_port);
            rc_endpoints_[id] = { rc_endpoint }; 
            
            LOG_MODULE_INFO(ZMQ_MODULE, "Connected to " << info.component_type << " " << id 
                      << " KPM: " << kpm_endpoint << " RC: " << rc_endpoint);
        }
        catch (const zmq::error_t& e) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "ZMQ error connecting to " << id << ": " << e.what());
            return false;
        }
    }
    
    return true;
}

void ZmqInterface::setKpmCallback(const std::string& component_id, 
                                std::function<void(const std::string&, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    kpm_callbacks_[component_id] = callback;
    LOG_MODULE_DEBUG(ZMQ_MODULE, "Set KPM callback for component " << component_id);
}

// Add to ZmqInterface class
bool ZmqInterface::sendCuDuplicationCommand(const std::string& command_params) {
    // Find the first CU component in ran_components_
    for (const auto& [id, info] : ran_components_) {
        if (info.component_type == "CU") {
            // Found a CU component, send command to it
            LOG_MODULE_INFO(ZMQ_MODULE, "Found CU component with ID: " << id);
            return sendRcCommand(id, "set_duplication", command_params);
        }
    }
    
    // No CU component found
    LOG_MODULE_ERROR(ZMQ_MODULE, "No CU component registered");
    return false;
}

bool ZmqInterface::sendCuCommand(const std::string& command_params) {
    // Try to parse as JSON to see if it's a structured command
    Json::Value root;
    Json::Reader reader;
    if (reader.parse(command_params, root)) {
        // If it has a "command" field, use that as the command type
        if (root.isMember("command")) {
            for (const auto& [id, info] : ran_components_) {
                if (info.component_type == "CU") {
                    std::string command_type = root["command"].asString();
                    LOG_MODULE_INFO(ZMQ_MODULE, "Detected command type from JSON: " << command_type);
                    return sendRcCommand(id, command_type, command_params);
                }
            }
        }
    }
    
    // Parse the command parameters to determine command type
    std::string command_type = "set_path";  // Default for backward compatibility
    
    // Find the first CU component in ran_components_
    for (const auto& [id, info] : ran_components_) {
        if (info.component_type == "CU") {
            // Found a CU component, send command to it
            LOG_MODULE_INFO(ZMQ_MODULE, "Found CU component with ID: " << id);
            return sendRcCommand(id, command_type, command_params);
        }
    }
    
    // No CU component found
    LOG_MODULE_ERROR(ZMQ_MODULE, "No CU component registered");
    return false;
}

zmq::socket_t& ZmqInterface::getThreadReqSocket(const std::string& id,
                                                const std::string& endpoint)
{
    thread_local std::unordered_map<std::string, zmq::socket_t> tls;
    auto it = tls.find(id);
    if (it == tls.end()) {
        zmq::socket_t s(context_, zmq::socket_type::req);
        s.set(zmq::sockopt::linger, 0);
        s.set(zmq::sockopt::rcvtimeo, 5000);
        s.connect(endpoint);
        it = tls.emplace(id, std::move(s)).first;
    }
    return it->second;
}

bool ZmqInterface::sendRcCommand(const std::string& component_id,
                                 const std::string& command_type,
                                 const std::string& command_params)
{
    // 3‑A. 엔드포인트 조회 (공유 map → lock 필요)
    std::string endpoint;
    {
        std::lock_guard<std::mutex> g(mutex_);
        auto e = rc_endpoints_.find(component_id);
        if (e == rc_endpoints_.end()) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "No RC endpoint for " << component_id);
            return false;
        }
        endpoint = e->second.addr;
    }

    // 3‑B. 스레드‑로컬 소켓 확보 (lock 없어도 됨)
    zmq::socket_t& sock = getThreadReqSocket(component_id, endpoint);

    // 3‑C. 메시지 직렬화 (필요하다면 잠깐 lock, 아니면 풀고 진행)
    RcCommandMessage cmd;
    cmd.component_id   = component_id;
    cmd.command_type   = command_type;
    cmd.command_params = command_params;
    cmd.timestamp      = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    std::string cmd_str = serializeRcCommand(cmd);

    // 3‑D. send / recv
    sock.send(zmq::buffer(cmd_str), zmq::send_flags::none);

    zmq::message_t reply;
    if (!sock.recv(reply, zmq::recv_flags::none)) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "No reply from " << component_id);
        return false;
    }

    // 3‑E. 파싱 및 후처리 (예전 코드 그대로)
    RcResponseMessage resp;
    if (!deserializeRcResponse(std::string(static_cast<char*>(reply.data()), reply.size()), resp)) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse RC response from " << component_id);
        return false;
    }

    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    kpm_processor_->processRcResponse(component_id, (now_us - cmd.timestamp)/2);
    return true;
}

bool ZmqInterface::startListening() {
    if (running_) {
        LOG_MODULE_INFO(ZMQ_MODULE, "Already listening");
        return true;
    }
    
    running_ = true;
    listen_thread_ = std::thread(&ZmqInterface::listenLoop, this);
    LOG_MODULE_INFO(ZMQ_MODULE, "Started listening for KPM metrics");
    return true;
}

void ZmqInterface::closeAllSockets()
{
    // kpm SUB sockets -------------------------------------------------------
    for (auto &kv : kpm_sockets_) {
        try { kv.second->close(); } catch (...) {}
    }
    kpm_sockets_.clear();

    // thread‑local REQ sockets ---------------------------------------------
    // every thread owns its own copy – close only the sockets that belong
    // to *this* thread; other threads will destroy theirs when they exit.
    thread_local static std::unordered_map<std::string, zmq::socket_t> *tls = nullptr;
    if (tls) {
        for (auto &kv : *tls) {
            try { kv.second.close(); } catch (...) {}
        }
        tls->clear();
    }
}

bool ZmqInterface::stopListening() {
    if (!running_) {
        return true;
    }
    
    running_ = false;

    // ① interrupt poll() immediately
    context_.shutdown();      // cppzmq‑2: same as zmq_ctx_shutdown()

    // ② close sockets that live in this object
    closeAllSockets();

    // ③ wait for the background thread to finish
    if (listen_thread_.joinable())
        listen_thread_.join();

    // ④ finally, terminate the context completely
    context_.close();
    LOG_MODULE_INFO(ZMQ_MODULE, "Stopped listening");
    return true;
 }
std::unique_ptr<zmq::socket_t> ZmqInterface::createSocket(SocketType type) {
    std::unique_ptr<zmq::socket_t> socket;
    
    switch (type) {
        case SocketType::PUB:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_PUB);
            break;
        case SocketType::SUB:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_SUB);
            break;
        case SocketType::REQ:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_REQ);
            break;
        case SocketType::REP:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_REP);
            break;
        case SocketType::DEALER:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_DEALER);
            break;
        case SocketType::ROUTER:
            socket = std::make_unique<zmq::socket_t>(context_, ZMQ_ROUTER);
            break;
        default:
            LOG_MODULE_ERROR(ZMQ_MODULE, "Unsupported socket type");
            throw std::runtime_error("Unsupported socket type");
    }
    
    // Set reasonable socket options
    socket->set(zmq::sockopt::linger, 1000); // 1 second linger period
    socket->set(zmq::sockopt::rcvtimeo, 5000); // 5 second receive timeout
    
    return socket;
}

void ZmqInterface::listenLoop() {
    LOG_MODULE_DEBUG(ZMQ_MODULE, "Listen loop started");
    
    zmq::pollitem_t items[10]; // Max 10 components to poll
    size_t item_count = 0;
    std::map<size_t, std::string> item_to_component;
    
    // Prepare poll items
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, socket] : kpm_sockets_) {
            if (item_count >= 10) break;
            
            items[item_count].socket = static_cast<void*>(*socket);
            items[item_count].events = ZMQ_POLLIN;
            item_to_component[item_count] = id;
            item_count++;
        }
    }
    
    while (running_) {
        try {
            // Poll with 100ms timeout
            zmq::poll(items, item_count, 100);
            
            for (size_t i = 0; i < item_count; i++) {
                if (items[i].revents & ZMQ_POLLIN) {
                    std::string component_id = item_to_component[i];
                    
                    // Receive message
                    zmq::message_t message;
                    auto socket = kpm_sockets_[component_id].get();
                    auto result = socket->recv(message, zmq::recv_flags::none);
                    
                    if (result) {
                        std::string msg_str(static_cast<char*>(message.data()), message.size());
                        LOG_MODULE_DEBUG(ZMQ_MODULE, "Received message: " << msg_str);
                        
                        // Extract and process the message
                        // The format is "component_id JSON_message"
                        std::string received_component_id;
                        std::string json_part;
                        
                        if (parsePrefixedMessage(msg_str, received_component_id, json_part)) {
                            // Check if the component ID matches
                            if (received_component_id == component_id) {
                                // Parse the JSON part
                                KpmMessage kpm;
                                if (deserializeKpmMessage(json_part, kpm)) {
                                    LOG_MODULE_DEBUG(ZMQ_MODULE, "Received KPM metric from " << component_id 
                                            << ": " << kpm.metric_type << " = " << kpm.metric_value);
                                    
                                    // Process metrics with KPM processor
                                    if (kpm_processor_) {
                                        // Check component type to determine which processing method to use
                                        auto component_it = ran_components_.find(component_id);
                                        if (component_it != ran_components_.end()) {
                                            if (component_it->second.component_type == "CU") {
                                                // Process as CU metrics
                                                if (kpm_processor_->processCuMetrics(component_id, kpm.metric_type, kpm.metric_value)) {
                                                    LOG_MODULE_DEBUG(ZMQ_MODULE, "Successfully processed CU KPM metrics from " << component_id);
                                                }
                                            } else {
                                                // Process as DU metrics (default)
                                                if (kpm_processor_->processDuMetrics(component_id, kpm.metric_type, kpm.metric_value)) {
                                                    LOG_MODULE_DEBUG(ZMQ_MODULE, "Successfully processed DU KPM metrics from " << component_id);
                                                }
                                            }
                                        } else {
                                            // If component type is unknown, try DU metrics processing
                                            if (kpm_processor_->processDuMetrics(component_id, kpm.metric_type, kpm.metric_value)) {
                                                LOG_MODULE_DEBUG(ZMQ_MODULE, "Successfully processed KPM metrics from " << component_id);
                                            }
                                        }
                                    }
                                    
                                    // Find and call callback
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    auto callback_it = kpm_callbacks_.find(component_id);
                                    if (callback_it != kpm_callbacks_.end()) {
                                        callback_it->second(kpm.metric_type, kpm.metric_value);
                                    }
                                } else {
                                    LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse KPM message from " << component_id);
                                }
                                                            } else {
                                LOG_MODULE_ERROR(ZMQ_MODULE, "Component ID mismatch: expected " << component_id 
                                         << ", received " << received_component_id);
                            }
                        } else {
                            LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse prefixed message from " << component_id);
                        }
                    }
                }
            }
        }
        catch (const zmq::error_t& e) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "ZMQ error in listen loop: " << e.what());
            // Brief pause before retrying
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LOG_MODULE_DEBUG(ZMQ_MODULE, "Listen loop ended");
}

// Message serialization
std::string ZmqInterface::serializeKpmMessage(const KpmMessage& msg) {
    Json::Value root;
    root["type"] = "kpm_metrics";
    root["component_id"] = msg.component_id;
    root["metric_type"] = msg.metric_type;
    root["metric_value"] = msg.metric_value;
    root["timestamp"] = Json::Value::UInt64(msg.timestamp);
    
    Json::FastWriter writer;
    return writer.write(root);
}

std::string ZmqInterface::serializeRcCommand(const RcCommandMessage& msg) {
    Json::Value root;
    root["type"] = "rc_command";
    root["component_id"] = msg.component_id;
    root["command_type"] = msg.command_type;
    root["command_params"] = msg.command_params;
    root["timestamp"] = Json::Value::UInt64(msg.timestamp);
    
    // Add multilink scheduling information if available
    if (!msg.link_ids.empty() && !msg.split_ratios.empty()) {
        Json::Value link_ids(Json::arrayValue);
        Json::Value split_ratios(Json::arrayValue);
        
        for (int link_id : msg.link_ids) {
            link_ids.append(link_id);
        }
        
        for (double ratio : msg.split_ratios) {
            split_ratios.append(ratio);
        }
        
        root["link_ids"] = link_ids;
        root["split_ratios"] = split_ratios;
        
        LOG_MODULE_INFO(ZMQ_MODULE, "Adding multilink info: " << msg.link_ids.size() << " links with split ratios");
    }
    
    Json::FastWriter writer;
    return writer.write(root);
}

// Helper function to parse prefixed messages (component_id JSON_message)
bool ZmqInterface::parsePrefixedMessage(const std::string& message, 
                                        std::string& component_id, 
                                        std::string& json_part) {
    // Find the first space which separates component_id from JSON
    size_t space_pos = message.find(' ');
    if (space_pos == std::string::npos) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "No space found in message: " << message);
        return false;
    }
    
    // Extract component_id and JSON part
    component_id = message.substr(0, space_pos);
    json_part = message.substr(space_pos + 1);
    
    if (component_id.empty() || json_part.empty()) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "Invalid message format, empty component_id or JSON part");
        return false;
    }
    
    return true;
}

// Modified deserializeKpmMessage to handle the JSON part only
bool ZmqInterface::deserializeKpmMessage(const std::string& data, KpmMessage& msg) {
    try {
        Json::Value root;
        Json::Reader reader;
        bool success = reader.parse(data, root);
        
        if (!success) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse KPM message: " << reader.getFormattedErrorMessages());
            return false;
        }
        
        if (root["type"].asString() != "kpm_metrics") {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Invalid message type: " << root["type"].asString());
            return false;
        }
        
        msg.component_id = root["component_id"].asString();
        msg.metric_type = root["metric_type"].asString();
        msg.metric_value = root["metric_value"].asString();
        msg.timestamp = root["timestamp"].asUInt64();
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "Error parsing KPM message: " << e.what());
        return false;
    }
}

bool ZmqInterface::deserializeRcCommand(const std::string& data, RcCommandMessage& msg) {
    try {
        Json::Value root;
        Json::Reader reader;
        bool success = reader.parse(data, root);
        
        if (!success) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse RC command: " << reader.getFormattedErrorMessages());
            return false;
        }
        
        if (root["type"].asString() != "rc_command") {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Invalid message type: " << root["type"].asString());
            return false;
        }
        
        msg.component_id = root["component_id"].asString();
        msg.command_type = root["command_type"].asString();
        msg.command_params = root["command_params"].asString();
        msg.timestamp = root["timestamp"].asUInt64();
        
        // Parse multilink scheduling information if available
        if (root.isMember("link_ids") && root.isMember("split_ratios")) {
            Json::Value link_ids = root["link_ids"];
            Json::Value split_ratios = root["split_ratios"];
            
            if (link_ids.isArray() && split_ratios.isArray() && 
                link_ids.size() == split_ratios.size() && link_ids.size() > 0) {
                
                msg.link_ids.clear();
                msg.split_ratios.clear();
                
                for (const auto& link_id : link_ids) {
                    msg.link_ids.push_back(link_id.asInt());
                }
                
                for (const auto& ratio : split_ratios) {
                    msg.split_ratios.push_back(ratio.asDouble());
                }
                
                LOG_MODULE_INFO(ZMQ_MODULE, "Received multilink info with " << msg.link_ids.size() << " links");
            } else {
                LOG_MODULE_WARN(ZMQ_MODULE, "Invalid multilink scheduling information in RC command");
            }
        }
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "Error parsing RC command: " << e.what());
        return false;
    }
}

bool ZmqInterface::deserializeRcResponse(const std::string& data, RcResponseMessage& msg) {
    try {
        Json::Value root;
        Json::Reader reader;
        bool success = reader.parse(data, root);
        
        if (!success) {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Failed to parse RC response: " << reader.getFormattedErrorMessages());
            return false;
        }
        
        if (root["type"].asString() != "rc_response") {
            LOG_MODULE_ERROR(ZMQ_MODULE, "Invalid message type: " << root["type"].asString());
            return false;
        }
        
        msg.component_id = root["component_id"].asString();
        msg.response_code = root["response_code"].asString();
        msg.response_data = root["response_data"].asString();
        msg.timestamp = root["timestamp"].asUInt64();
        msg.sender_timestamp = root["sender_timestamp"].asUInt64();
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(ZMQ_MODULE, "Error parsing RC response: " << e.what());
        return false;
    }
}

} // namespace ric