#include "RIC/ric_interface_wrapper.hpp"
#include "log.hpp"
#include <json-c/json.h>
#include <cstring> // For memcpy, etc
#include <string.h> // For strdup

// Define strdup for C++
#ifndef strdup
#define strdup(s) \
    strcpy((char*)malloc(strlen(s)+1), s)
#endif

namespace ric {

RicInterfaceWrapper::RicInterfaceWrapper(
    RicComponentType component_type,
    const std::string& component_id,
    int kpm_port,
    int rc_port,
    const std::string& ric_ip,
    int ric_kpm_port,
    int ric_rc_port
) : handle_(nullptr),
    component_type_(component_type),
    component_id_(component_id),
    kpm_port_(kpm_port),
    rc_port_(rc_port),
    ric_ip_(ric_ip),
    ric_kpm_port_(ric_kpm_port),
    ric_rc_port_(ric_rc_port),
    initialized_(false),
    running_(false)
{
}

RicInterfaceWrapper::~RicInterfaceWrapper() {
    if (running_) {
        stop();
    }
    
    if (handle_) {
        RIC_ZMQ_Free(handle_);
        handle_ = nullptr;
    }
}

bool RicInterfaceWrapper::initInNamespace(const std::string& nsName) {
    // 1) Save the host netns

    // 3) Now that we are inside nsName, do the normal “initialize()” steps
    bool ok = this->initialize();  // calls RIC_ZMQ_Init, etc.
    if (ok) {
        // optionally start it immediately
        ok = this->start();
    }

    LOG_INFO("[RIC] Done!");

    return ok;
}

bool RicInterfaceWrapper::initialize() {
    if (initialized_) {
        LOG_WARN("[RIC] Interface already initialized");
        return true;
    }
    
    handle_ = RIC_ZMQ_Init(
        component_type_,
        component_id_.c_str(),
        kpm_port_,
        rc_port_,
        ric_ip_.c_str(),
        ric_kpm_port_,
        ric_rc_port_
    );
    
    if (!handle_) {
        LOG_ERROR("[RIC] Failed to initialize ZMQ interface: " << RIC_ZMQ_GetLastError());
        return false;
    }
    
    if (!RIC_ZMQ_SetRcCallback(handle_, handleCommand, this)) {
        LOG_ERROR("[RIC] Failed to set RC callback: " << RIC_ZMQ_GetLastError());
        RIC_ZMQ_Free(handle_);
        handle_ = nullptr;
        return false;
    }
    
    initialized_ = true;
    LOG_INFO("[RIC] Interface initialized successfully");
    return true;
}

bool RicInterfaceWrapper::start() {
    if (!initialized_) {
        LOG_ERROR("[RIC] Cannot start uninitialized interface");
        return false;
    }
    
    if (running_) {
        LOG_WARN("[RIC] Interface already running");
        return true;
    }
    
    if (!RIC_ZMQ_Start(handle_)) {
        LOG_ERROR("[RIC] Failed to start ZMQ interface: " << RIC_ZMQ_GetLastError());
        return false;
    }
    
    running_ = true;
    LOG_INFO("[RIC] Interface started");
    return true;
}

bool RicInterfaceWrapper::stop() {
    if (!running_) {
        return true;
    }
    
    if (!RIC_ZMQ_Stop(handle_)) {
        LOG_ERROR("[RIC] Failed to stop ZMQ interface: " << RIC_ZMQ_GetLastError());
        return false;
    }
    
    running_ = false;
    LOG_INFO("[RIC] Interface stopped");
    return true;
}

bool RicInterfaceWrapper::sendKpmMetric(const std::string& metric_type, const std::string& metric_value) {
    if (!initialized_) {
        LOG_ERROR("[RIC] Cannot send metric with uninitialized interface");
        return false;
    }
    
    if (!RIC_ZMQ_SendKpmMetric(handle_, metric_type.c_str(), metric_value.c_str())) {
        LOG_ERROR("[RIC] Failed to send KPM metric: " << RIC_ZMQ_GetLastError());
        return false;
    }
    
    LOG_DEBUG("[RIC] Sent KPM metric: " << metric_type);
    return true;
}

bool RicInterfaceWrapper::registerCommandHandler(
    const std::string& command_type,
    std::function<std::string(const std::string&)> handler
) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    command_handlers_[command_type] = handler;
    LOG_INFO("[RIC] Registered handler for command type: " << command_type);
    return true;
}

std::string RicInterfaceWrapper::getLastError() const {
    return RIC_ZMQ_GetLastError();
}

char* RicInterfaceWrapper::handleCommand(const char* command_type, const char* command_params, void* user_data) {
    RicInterfaceWrapper* wrapper = static_cast<RicInterfaceWrapper*>(user_data);
    
    std::string cmd_type(command_type);
    std::string params(command_params);
    
    LOG_INFO("[RIC] Received command: " << cmd_type);
    LOG_DEBUG("[RIC] Command parameters: " << params);
    
    // Default response
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "status", json_object_new_string("error"));
    json_object_object_add(response, "message", json_object_new_string("Command handler not found"));
    
    // Find handler for this command type
    std::string handler_result;
    {
        std::lock_guard<std::mutex> lock(wrapper->handlers_mutex_);
        auto it = wrapper->command_handlers_.find(cmd_type);
        if (it != wrapper->command_handlers_.end()) {
            try {
                // Call the handler
                handler_result = it->second(params);
                
                // Clean up default response
                json_object_put(response);
                
                // Use handler result directly
                return strdup(handler_result.c_str());
            }
            catch (const std::exception& e) {
                LOG_ERROR("[RIC] Exception in command handler: " << e.what());
                json_object_put(response);
                
                // Create error response
                response = json_object_new_object();
                json_object_object_add(response, "status", json_object_new_string("error"));
                json_object_object_add(response, "message", 
                                      json_object_new_string(
                                          std::string("Exception in handler: ").append(e.what()).c_str()
                                      ));
            }
        }
    }
    
    // Convert response to string
    const char* response_str = json_object_to_json_string(response);
    char* result = strdup(response_str);
    
    // Clean up
    json_object_put(response);
    
    return result;
}

} // namespace ric