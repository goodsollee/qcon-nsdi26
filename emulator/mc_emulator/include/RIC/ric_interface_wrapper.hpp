#pragma once

#include "ric_zmq_interface.h"
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include "namespace.h"
#include <json-c/json.h>

namespace ric {

/**
 * C++ wrapper for the RIC ZMQ interface
 */
class RicInterfaceWrapper {
public:
    /**
     * Constructor
     * @param component_type Type of RAN component
     * @param component_id Unique identifier for this component
     * @param kpm_port Port for KPM metrics publication
     * @param rc_port Port for RC command reception
     * @param ric_ip IP address of the RIC
     * @param ric_kpm_port RIC's port for receiving KPM metrics
     * @param ric_rc_port RIC's port for sending RC commands
     */
    RicInterfaceWrapper(
        RicComponentType component_type,
        const std::string& component_id,
        int kpm_port,
        int rc_port,
        const std::string& ric_ip,
        int ric_kpm_port,
        int ric_rc_port
    );

    /**
     * Destructor
     */
    ~RicInterfaceWrapper();

    bool initInNamespace(const std::string& nsName);

    /**
     * Initialize the interface
     * @return true on success, false on failure
     */
    bool initialize();

    /**
     * Start the interface
     * @return true on success, false on failure
     */
    bool start();

    /**
     * Stop the interface
     * @return true on success, false on failure
     */
    bool stop();

    /**
     * Send a KPM metric to the RIC
     * @param metric_type Type of metric
     * @param metric_value JSON string with metric value
     * @return true on success, false on failure
     */
    bool sendKpmMetric(const std::string& metric_type, const std::string& metric_value);

    /**
     * Register a command handler
     * @param command_type Type of command to handle
     * @param handler Function to call when command is received
     * @return true on success, false on failure
     */
    bool registerCommandHandler(
        const std::string& command_type,
        std::function<std::string(const std::string&)> handler
    );

    /**
     * Get the last error message
     * @return Error message
     */
    std::string getLastError() const;

    bool isRunning () {return running_;}

    // Add to RicInterfaceWrapper class
    RICZmqHandle getHandle() const {
        return handle_;
    }

private:
    RICZmqHandle handle_;
    RicComponentType component_type_;
    std::string component_id_;
    int kpm_port_;
    int rc_port_;
    std::string ric_ip_;
    int ric_kpm_port_;
    int ric_rc_port_;
    bool initialized_;
    bool running_;

    // Command handlers
    std::map<std::string, std::function<std::string(const std::string&)>> command_handlers_;
    std::mutex handlers_mutex_;

    // Static callback function for C API
    static char* handleCommand(const char* command_type, const char* command_params, void* user_data);
};

} // namespace ric