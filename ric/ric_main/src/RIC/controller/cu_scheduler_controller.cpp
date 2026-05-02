#include "RIC/controller/cu_scheduler_controller.hpp"
#include <iostream>
#include <chrono>

const std::string MODULE = "CU_SCHEDULER_CONTROLLER";

namespace ric {

CuSchedulerController::CuSchedulerController(ZmqInterface& zmq_interface)
    : zmq_interface_(zmq_interface), running_(false) {

    start();
}

CuSchedulerController::~CuSchedulerController() {
    stop();
}

bool CuSchedulerController::initialize() {
    // No specific initialization needed for now
    LOG_MODULE_INFO(MODULE, "CU Scheduler Controller initialized");
    return true;
}

bool CuSchedulerController::start() {
    if (running_) {
        LOG_MODULE_WARN(MODULE, "CU Scheduler Controller already running");
        return true;
    }
    
    running_ = true;
    //worker_thread_ = std::thread(&CuSchedulerController::workerThread, this);
    
    LOG_MODULE_INFO(MODULE, "CU Scheduler Controller started");
    return true;
}

void CuSchedulerController::stop() {
    if (!running_) {
        return;
    }
    
    LOG_MODULE_INFO(MODULE, "Stopping CU Scheduler Controller");
    running_ = false;
    
    // Wake up worker thread
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        command_cv_.notify_all();
    }
    
    // Wait for worker thread to finish
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    
    LOG_MODULE_INFO(MODULE, "CU Scheduler Controller stopped");
}

bool CuSchedulerController::sendDuplicationCommand(const Json::Value& params) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot set active path: controller not running");
        return false;
    }
    
    // Convert JsonCpp value to string using JsonCpp's methods
    std::string params_str = params.toStyledString();
    // Or use this for a more compact representation without whitespace:
    // Json::FastWriter writer;
    // std::string params_str = writer.write(params);
    
    bool result = zmq_interface_.sendCuDuplicationCommand(params_str.c_str());
    
    // No need for json_object_put() as JsonCpp objects are managed differently
    
    return result;
}

bool CuSchedulerController::sendReinjectBurst(int alt_path, int burst_packets) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot send reinject_burst: controller not running");
        return false;
    }
    // Construct JSON params with explicit "command" field so sendCuCommand can
    // route via the CU component (pdcp_sender). Earlier we iterated
    // active_paths_, but that map is only populated AFTER a set_path has been
    // sent — on traces where path didn't change, active_paths_ stays empty
    // and the loop does 0 iterations → any_ok=0 → sender never receives.
    Json::Value params;
    params["command"]       = "reinject_burst";
    params["alt_path"]      = alt_path;
    params["burst_packets"] = burst_packets;
    std::string params_str  = Json::FastWriter().write(params);
    bool ok = zmq_interface_.sendCuCommand(params_str);
    // ERROR-level so it's visible at LOG=ERROR (default) — burst is a
    // visible data-plane action and we need to audit every fire.
    LOG_MODULE_ERROR(MODULE, "reinject_burst sent (ok=" << ok
                    << "): alt_path=" << alt_path << " burst=" << burst_packets);
    return ok;
}

bool CuSchedulerController::sendCommandImmediate(int path) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot set active path: controller not running");
        return false;
    }

    // Create command parameters as JSON
    struct json_object* params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_int(path));

    const char* params_str = json_object_to_json_string(params);
    bool result = zmq_interface_.sendCuCommand(params_str);

    // Clean up
    json_object_put(params);

    return result;
}

bool CuSchedulerController::sendMultilinkCommandImmediate(
        const std::map<int, double>& split_ratio_zero_indexed) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot set multilink path: controller not running");
        return false;
    }
    if (split_ratio_zero_indexed.empty()) {
        LOG_MODULE_WARN(MODULE, "sendMultilinkCommandImmediate called with empty split");
        return false;
    }

    // Build same JSON shape that handleSetPath multilink branch expects:
    //   { "is_multilink": true, "link_ids": [0,1,...], "split_ratios": [r0,r1,...] }
    // link_ids are already 0-indexed (matching mc_emulator's path numbering).
    struct json_object* params = json_object_new_object();
    json_object_object_add(params, "is_multilink", json_object_new_boolean(true));
    struct json_object* lids = json_object_new_array();
    struct json_object* rats = json_object_new_array();
    double sum = 0.0;
    for (const auto& kv : split_ratio_zero_indexed) sum += kv.second;
    if (sum <= 0.0) sum = 1.0;
    for (const auto& kv : split_ratio_zero_indexed) {
        json_object_array_add(lids, json_object_new_int(kv.first));
        json_object_array_add(rats, json_object_new_double(kv.second / sum));
    }
    json_object_object_add(params, "link_ids", lids);
    json_object_object_add(params, "split_ratios", rats);

    const char* params_str = json_object_to_json_string(params);
    bool result = zmq_interface_.sendCuCommand(params_str);
    json_object_put(params);
    return result;
}

bool CuSchedulerController::setActivePath(const std::string& component_id, int path) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot set active path: controller not running");
        return false;
    }
    
    LOG_MODULE_INFO(MODULE, "Queueing set active path command for " << component_id << ": path=" << path);
    
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        command_queue_.emplace(component_id, path);
        command_cv_.notify_one();
    }
    
    return true;
}

bool CuSchedulerController::getActivePath(const std::string& component_id, int& path) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = active_paths_.find(component_id);
    if (it == active_paths_.end()) {
        return false;
    }
    
    path = it->second;
    return true;
}

std::vector<std::string> CuSchedulerController::getRegisteredComponents() {
    std::vector<std::string> components;
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Add all components that have an active path setting
    for (const auto& pair : active_paths_) {
        components.push_back(pair.first);
    }
    
    return components;
}

void CuSchedulerController::registerKpmCallback(
    const std::string& component_id, 
    std::function<void(const std::string&, const std::string&)> callback
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    kpm_callbacks_[component_id] = callback;
    
    // Register callback with ZMQ interface
    zmq_interface_.setKpmCallback(component_id, 
        [this, component_id](const std::string& metric_type, const std::string& metric_value) {
            this->processKpmMetric(component_id, metric_type, metric_value);
        }
    );
    
    LOG_MODULE_INFO(MODULE, "Registered KPM callback for component " << component_id);
}

void CuSchedulerController::workerThread() {
    LOG_MODULE_INFO(MODULE, "CU Scheduler worker thread started");
    
    while (running_) {
        PathControlCommand cmd("", 0);
        bool has_command = false;
        
        // Wait for a command
        {
            std::unique_lock<std::mutex> lock(command_mutex_);
            if (command_queue_.empty()) {
                // Wait with timeout
                command_cv_.wait_for(lock, std::chrono::seconds(1), 
                    [this]() { return !running_ || !command_queue_.empty(); });
                
                if (!running_) {
                    break;
                }
                
                if (command_queue_.empty()) {
                    continue;
                }
            }
            
            // Get the next command
            cmd = command_queue_.front();
            command_queue_.pop();
            has_command = true;
        }
        
        if (has_command) {
            if (cmd.is_multilink) {
                LOG_MODULE_INFO(MODULE, "Processing multilink path command for " << cmd.component_id
                             << " with " << cmd.link_ids.size() << " links");
                
                // Send multilink command
                if (sendMultilinkPathCommand(cmd.component_id, cmd.link_ids, cmd.split_ratios)) {
                    LOG_MODULE_INFO(MODULE, "Successfully sent multilink path command");
                    
                    // Update active path state with first link ID for backward compatibility
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!cmd.link_ids.empty()) {
                        active_paths_[cmd.component_id] = cmd.link_ids[0];
                    }
                } else {
                    LOG_MODULE_ERROR(MODULE, "Failed to send multilink path command");
                }
            } else {
                LOG_MODULE_INFO(MODULE, "Processing set active path command for " 
                             << cmd.component_id << ": path=" << cmd.path);
                
                // Send single-link command
                if (sendPathControlCommand(cmd.component_id, cmd.path)) {
                    LOG_MODULE_INFO(MODULE, "Successfully sent path control command");
                    
                    // Update active path state
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    active_paths_[cmd.component_id] = cmd.path;
                } else {
                    LOG_MODULE_ERROR(MODULE, "Failed to send path control command");
                }
            }
        }
    }
    
    LOG_MODULE_INFO(MODULE, "CU Scheduler worker thread stopped");
}

bool CuSchedulerController::sendPathControlCommand(const std::string& component_id, int path) {
    // Create command parameters as JSON
    Json::Value params;
    params["path"] = path;
    params["is_multilink"] = false;
    params["action"] = "set_active_path";
    
    std::string params_str = Json::FastWriter().write(params);
    bool result = zmq_interface_.sendRcCommand(component_id, "set_path", params_str);
    
    if (result) {
        LOG_MODULE_INFO(MODULE, "Sent single-link path control command to " << component_id << ": path=" << path);
    } else {
        LOG_MODULE_ERROR(MODULE, "Failed to send single-link path control command to " << component_id);
    }
    
    return result;
}

bool CuSchedulerController::setMultilinkPath(const std::string& component_id, 
                                          const std::vector<int>& link_ids,
                                          const std::vector<double>& split_ratios) {
    if (!running_) {
        LOG_MODULE_ERROR(MODULE, "Cannot set multilink path: controller not running");
        return false;
    }
    
    if (link_ids.empty() || split_ratios.empty() || link_ids.size() != split_ratios.size()) {
        LOG_MODULE_ERROR(MODULE, "Invalid multilink configuration: links=" << link_ids.size() 
                       << ", ratios=" << split_ratios.size());
        return false;
    }
    
    LOG_MODULE_INFO(MODULE, "Queueing multilink path command for " << component_id 
                  << " with " << link_ids.size() << " links");
    
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        command_queue_.emplace(component_id, link_ids, split_ratios);
        command_cv_.notify_one();
    }
    
    return true;
}

bool CuSchedulerController::sendMultilinkPathCommand(const std::string& component_id, 
                                                  const std::vector<int>& link_ids,
                                                  const std::vector<double>& split_ratios) {
    // Validation
    if (link_ids.empty() || split_ratios.empty() || link_ids.size() != split_ratios.size()) {
        LOG_MODULE_ERROR(MODULE, "Invalid multilink configuration: links=" << link_ids.size() 
                       << ", ratios=" << split_ratios.size());
        return false;
    }
    
    // Calculate sum of ratios for normalization if needed
    double total_ratio = 0.0;
    for (double ratio : split_ratios) {
        total_ratio += ratio;
    }
    
    // Create JSON command
    Json::Value params;
    params["is_multilink"] = true;
    params["action"] = "set_multilink_path";
    
    // Add link IDs
    Json::Value links(Json::arrayValue);
    for (int link_id : link_ids) {
        links.append(link_id);
    }
    params["link_ids"] = links;
    
    // Add split ratios (normalized if needed)
    Json::Value ratios(Json::arrayValue);
    if (std::abs(total_ratio - 1.0) > 0.001) {
        LOG_MODULE_WARN(MODULE, "Split ratios sum to " << total_ratio << ", normalizing to 1.0");
        for (double ratio : split_ratios) {
            ratios.append(ratio / total_ratio);
        }
    } else {
        for (double ratio : split_ratios) {
            ratios.append(ratio);
        }
    }
    params["split_ratios"] = ratios;
    
    // Send command
    std::string params_str = Json::FastWriter().write(params);
    
    RcCommandMessage msg;
    msg.component_id = component_id;
    msg.command_type = "set_path";
    msg.command_params = params_str;
    msg.link_ids = link_ids;
    msg.split_ratios = split_ratios;
    
    // Use custom sendRcCommand to include multilink data
    bool result = zmq_interface_.sendRcCommand(component_id, "set_path", params_str);
    
    if (result) {
        LOG_MODULE_INFO(MODULE, "Sent multilink path command to " << component_id 
                       << " with " << link_ids.size() << " links");
        
        // Update active path state with first link for backward compatibility
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!link_ids.empty()) {
            active_paths_[component_id] = link_ids[0];
        }
    } else {
        LOG_MODULE_ERROR(MODULE, "Failed to send multilink path command to " << component_id);
    }
    
    return result;
}

void CuSchedulerController::processKpmMetric(
    const std::string& component_id, 
    const std::string& metric_type, 
    const std::string& metric_value
) {
    LOG_MODULE_DEBUG(MODULE, "Processing KPM metric from " << component_id << ": " << metric_type);
    
    // Check if we need to update active path state
    if (metric_type == "pdcp_path_stats") {
        int active_path = 0;
        if (parsePathStats(metric_value, active_path)) {
            // Update state
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_paths_[component_id] = active_path;
            LOG_MODULE_DEBUG(MODULE, "Updated active path for " << component_id << ": " << active_path);
        }
    }
    
    // Forward to registered callback
    std::function<void(const std::string&, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = kpm_callbacks_.find(component_id);
        if (it != kpm_callbacks_.end()) {
            callback = it->second;
        }
    }
    
    if (callback) {
        try {
            callback(metric_type, metric_value);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Exception in KPM callback: " << e.what());
        }
    }
}

bool CuSchedulerController::parsePathStats(const std::string& metric_value, int& active_path) {
    try {
        struct json_object* root = json_tokener_parse(metric_value.c_str());
        if (!root) {
            LOG_MODULE_ERROR(MODULE, "Failed to parse path stats JSON");
            return false;
        }
        
        // Extract active path
        struct json_object* active_path_obj;
        if (json_object_object_get_ex(root, "active_path", &active_path_obj)) {
            active_path = json_object_get_int(active_path_obj);
        } else {
            json_object_put(root);
            return false;
        }
        
        // Clean up
        json_object_put(root);
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Exception parsing path stats: " << e.what());
        return false;
    }
}

} // namespace ric