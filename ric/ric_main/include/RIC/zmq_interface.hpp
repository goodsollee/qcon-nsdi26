// include/RIC/zmq_interface.hpp
#pragma once
#include <zmq.hpp>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include "log.hpp"
#include <jsoncpp/json/json.h>
#include "RIC/kpm_processor.hpp"
#include "state_manager/state_manager.hpp"

namespace ric {

// Socket types enumeration
enum class SocketType {
    PUB,
    SUB,
    REQ,
    REP,
    DEALER,
    ROUTER
};

// Connection information for a RAN component
struct RanComponentInfo {
    std::string component_type;  // "CU", "DU"
    std::string component_id;    // Unique identifier
    std::string ip_address;      // IP address of the component
    int kpm_port;                // Port for KPM metrics
    int rc_port;                 // Port for RC commands
};

// Message types for KPM and RC communication
enum class MessageType {
    KPM_METRICS,
    RC_COMMAND,
    RC_RESPONSE
};

// Base message structure
struct Message {
    MessageType type;
    std::string component_id;
    uint64_t timestamp;
    
    virtual ~Message() = default;
};

// KPM metrics message
struct KpmMessage : public Message {
    std::string metric_type;
    std::string metric_value;
    
    KpmMessage() {
        type = MessageType::KPM_METRICS;
    }
};

// RC command message
struct RcCommandMessage : public Message {
    std::string command_type;
    std::string command_params;
    
    // For multilink scheduling
    std::vector<int> link_ids;
    std::vector<double> split_ratios;
    
    RcCommandMessage() {
        type = MessageType::RC_COMMAND;
    }
};

// RC response message
struct RcResponseMessage : public Message {
    std::string response_code;
    std::string response_data;
    uint64_t sender_timestamp;
    
    RcResponseMessage() {
        type = MessageType::RC_RESPONSE;
        sender_timestamp = 0;
    }
};

// Base ZMQ interface class for RIC
class ZmqInterface {
public:
    ZmqInterface();
    ~ZmqInterface();

    // Initialize with RIC's own IP address
    bool initialize(const std::string& ric_ip);

    // Register a RAN component for communication
    bool registerRanComponent(const RanComponentInfo& component_info);

    // Connect to all registered RAN components
    bool connectToRanComponents();

    // Set callback for receiving KPM metrics from a RAN component
    void setKpmCallback(const std::string& component_id, 
                       std::function<void(const std::string&, const std::string&)> callback);

    bool sendCuDuplicationCommand(const std::string& command_params);

    bool sendCuCommand (const std::string& command_params);

    zmq::socket_t& getThreadReqSocket(const std::string& id, const std::string& endpoint);

    // Send RC command to a specific RAN component
    bool sendRcCommand(const std::string& component_id, const std::string& command_type, 
                        const std::string& command_params);

    // Start listening for incoming messages
    bool startListening();

    // Stop listening
    bool stopListening();

    void closeAllSockets();
    
    // Initialize KPM processor with a specified log directory
    bool registerKpmProcessor(std::shared_ptr<KpmProcessor> kpm_processor);

private:
    zmq::context_t context_;
    std::string ric_ip_;

    // Maps component ID to its connection information
    std::map<std::string, RanComponentInfo> ran_components_;

    // Maps component ID to its sockets
    std::map<std::string, std::unique_ptr<zmq::socket_t>> kpm_sockets_;
    
    struct RcEndpoint { std::string addr; };
    std::unordered_map<std::string, RcEndpoint> rc_endpoints_;

    // Maps component ID to its KPM callback
    std::map<std::string, std::function<void(const std::string&, const std::string&)>> kpm_callbacks_;

    // Thread for listening to messages
    std::thread listen_thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    
    // KPM processor for handling metrics
    std::shared_ptr<KpmProcessor> kpm_processor_;

    // ZMQ socket creation helper
    std::unique_ptr<zmq::socket_t> createSocket(SocketType type);

    // Listening thread function
    void listenLoop();
    
    // Message serialization/deserialization
    std::string serializeKpmMessage(const KpmMessage& msg);
    std::string serializeRcCommand(const RcCommandMessage& msg);
    bool parsePrefixedMessage(const std::string& message, 
                                        std::string& component_id, 
                                        std::string& json_part);
    bool deserializeKpmMessage(const std::string& data, KpmMessage& msg);
    bool deserializeRcCommand(const std::string& data, RcCommandMessage& msg);
    bool deserializeRcResponse(const std::string& data, RcResponseMessage& msg);
};

} // namespace ric