#include "RIC/controller/du_scheduler_controller.hpp"
#include <jsoncpp/json/json.h>
#include <chrono>
#include <thread>

const std::string MODULE = "DU_SCHEDULER_CONTROLLER";

namespace ric {

DuSchedulerController::DuSchedulerController(ZmqInterface& zmq_interface)
    : zmq_interface_(zmq_interface), running_(false) {
}

DuSchedulerController::~DuSchedulerController() {
    stop();
}

bool DuSchedulerController::initialize() {
    LOG_MODULE_INFO(MODULE, "DU Scheduler Controller initialized");
    return true;
}

bool DuSchedulerController::start() {
    if (running_) {
        LOG_MODULE_WARN(MODULE, "DU Scheduler Controller already running");
        return true;
    }
    
    running_ = true;
    LOG_MODULE_INFO(MODULE, "DU Scheduler Controller started");
    
    // Send start trace command to PHY sender components with a slight delay
    // to ensure connections are established
    std::thread([this]() {
        // Wait for 3 seconds before sending start trace command
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // List of PHY sender components to start
        std::vector<std::string> phy_components = {"phy_sender_1", "phy_sender_2"};
        
        for (const auto& component : phy_components) {
            if (sendStartTraceCommand(component)) {
                LOG_MODULE_INFO(MODULE, "Successfully sent start trace command to " << component);
            } else {
                LOG_MODULE_ERROR(MODULE, "Failed to send start trace command to " << component);
            }
        }
    }).detach();
    
    return true;
}

void DuSchedulerController::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    LOG_MODULE_INFO(MODULE, "DU Scheduler Controller stopped");
}

bool DuSchedulerController::sendStartTraceCommand(const std::string& component_id) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot send start trace command: controller not running");
        return false;
    }
    
    // Create empty JSON params
    Json::Value params;
    params["action"] = "start_trace";
    
    std::string params_str = Json::FastWriter().write(params);
    return sendCommand(component_id, "trace_control", params_str);
}

bool DuSchedulerController::sendCommand(const std::string& component_id,
                                      const std::string& command_type,
                                      const std::string& params) {
    // Send the command via ZMQ interface
    bool success = zmq_interface_.sendRcCommand(component_id, command_type, params);
    
    if (success) {
        LOG_MODULE_INFO(MODULE, "Command sent: type=" << command_type 
                      << ", component=" << component_id);
    } else {
        LOG_MODULE_ERROR(MODULE, "Failed to send command to " << component_id 
                       << ": " << command_type);
    }
    
    return success;
}

} // namespace ric