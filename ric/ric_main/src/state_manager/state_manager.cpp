#include "state_manager/state_manager.hpp"
#include "log.hpp"
#include <algorithm>
#include <chrono>

const std::string MODULE = "CROSS_LAYER_STATE_MANAGER";

namespace ric {

CrossLayerStateManager::CrossLayerStateManager() {
    LOG_MODULE_INFO(MODULE, "CrossLayerStateManager created");
}

CrossLayerStateManager::~CrossLayerStateManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.clear();
    link_states_.clear();
    LOG_MODULE_INFO(MODULE, "CrossLayerStateManager destroyed");
}

bool CrossLayerStateManager::initialize() {
    LOG_MODULE_INFO(MODULE, "Initializing CrossLayerStateManager");
    return true;
}

bool CrossLayerStateManager::processMetrics(const std::string& component_id,
                                          const std::string& component_type,
                                          const std::string& metric_type,
                                          const std::string& metric_value) {
    try {
        // Parse JSON metrics
        Json::Value metrics;
        Json::Reader reader;
        if (!reader.parse(metric_value, metrics)) {
            LOG_MODULE_ERROR(MODULE, "Failed to parse metrics JSON from " << component_id);
            return false;
        }
        
        // Get link ID for this component
        int link_id = getLinkIdFromComponent(component_id, component_type);
        
        // Process different metric types
        if (metric_type == "rlc_performance_metrics") {
            processRlcPerformanceMetrics(link_id, metrics);
        } else {
            LOG_MODULE_DEBUG(MODULE, "Ignoring unsupported metric type: " << metric_type);
        }

        if (metric_type == "pdcp_path_stats") {
            // Process PDCP stats if needed
            processPdcpPerformanceMetrics(metrics);
            LOG_MODULE_DEBUG(MODULE, "Processing PDCP stats from " << component_id);
        } else {
            LOG_MODULE_DEBUG(MODULE, "Ignoring unsupported metric type: " << metric_type);
        }
        
        // Notify system state updated
        notifySystemStateUpdated();
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Exception processing metrics from " << component_id << ": " << e.what());
        return false;
    }
}

void CrossLayerStateManager::processRlcPerformanceMetrics(int link_id, const Json::Value& metrics) {

    std::lock_guard<std::mutex> lock(mutex_);
    
    // Get or create link state
    auto it = link_states_.find(link_id);
    if (it == link_states_.end()) {
        LOG_MODULE_INFO(MODULE, "Creating new link state for " << link_id);
        link_states_[link_id] = std::make_shared<LinkState>(link_id);
        it = link_states_.find(link_id);
    }
    
    // Update link state with metrics
    it->second->updateFromRlcMetrics(metrics);
    
    // Notify listeners
    //notifyLinkStateUpdated(link_id, *it->second);
}

void CrossLayerStateManager::processPdcpPerformanceMetrics(const Json::Value& metrics) {

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (metrics.isMember("path_bytes") && metrics["path_bytes"].isObject()) {
        const Json::Value& pb = metrics["path_bytes"];
        for (const auto& memberName : pb.getMemberNames()) {
            // expect keys exactly “path_0”, “path_1”, … where the number is the link ID
            if (memberName.rfind("path_", 0) != 0) 
                continue;

            int link_id = std::stoi(memberName.substr(5)) + 1;         // strip “path_”
            int64_t bytes = pb[memberName].asInt64();

            // get‑or‑create the LinkState for this link_id
            auto it = link_states_.find(link_id);
            if (it == link_states_.end()) {
                LOG_MODULE_INFO(MODULE, "Creating new link state for " << link_id);
                link_states_[link_id] = std::make_shared<LinkState>(link_id);
                it = link_states_.find(link_id);
            }

            // update PDCP‐level counters on that link
            it->second->updatePdcpMetrics(bytes);
        }
    }
    
    // Notify listeners
    //notifyLinkStateUpdated(link_id, *it->second);
}

int CrossLayerStateManager::getLinkIdFromComponent(const std::string& component_id, const std::string& component_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if we already have a mapping
    auto it = component_to_link_map_.find(component_id);
    if (it != component_to_link_map_.end()) {
        return it->second;  // Now directly returning the int value
    }
    
    // Create a new mapping
    int link_id;
    
    if (component_type == "DU") {
        // For DU components, extract number from pattern "pdcp_du_linkX"
        size_t link_pos = component_id.find("link");
        if (link_pos != std::string::npos && link_pos + 4 < component_id.length()) {
            // Try to extract the number after "link"
            std::string num_part = component_id.substr(link_pos + 4);
            // Find where the number ends (in case there's something after it)
            size_t num_end = 0;
            while (num_end < num_part.length() && std::isdigit(num_part[num_end])) {
                num_end++;
            }
            if (num_end > 0) {
                link_id = std::stoi(num_part.substr(0, num_end));
            } else {
                // Fallback: Generate a new link ID based on the number of existing links
                link_id = link_states_.size() + 1;
            }
        } else {
            // Generate a new link ID based on the number of existing links
            link_id = link_states_.size() + 1;
        }
    } else {
        // For other components, generate a unique ID
        // This could be improved based on your specific requirements
        link_id = -1 * (component_ids_.size() + 1);  // Negative to distinguish from DU links
    }
    
    // Store the mapping (now directly storing the int)
    component_to_link_map_[component_id] = link_id;
    LOG_MODULE_INFO(MODULE, "Mapped component " << component_id << " to link ID " << link_id);
    
    return link_id;
}

std::shared_ptr<LinkState> CrossLayerStateManager::getLinkState(int link_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = link_states_.find(link_id);
    if (it != link_states_.end()) {
        return it->second;
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<LinkState>> CrossLayerStateManager::getActiveLinkStates() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<LinkState>> active_links;
    
    for (const auto& [link_id, link_state] : link_states_) {
        if (link_state->isActive()) {
            active_links.push_back(link_state);
        }
    }
    
    return active_links;
}

void CrossLayerStateManager::registerListener(StateUpdateListener* listener) {
    if (!listener) {
        return;
    }

    // Check if listener is already registered
    auto it = std::find(listeners_.begin(), listeners_.end(), listener);
    if (it == listeners_.end()) {
        listeners_.push_back(listener);
        LOG_MODULE_DEBUG(MODULE, "Registered state update listener");
        
        // Notify the new listener of the current state
        notifySystemStateUpdated();
    }
}

void CrossLayerStateManager::unregisterListener(StateUpdateListener* listener) {
    if (!listener) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find(listeners_.begin(), listeners_.end(), listener);
    if (it != listeners_.end()) {
        listeners_.erase(it);
        LOG_MODULE_DEBUG(MODULE, "Unregistered state update listener");
    }
}

Json::Value CrossLayerStateManager::getSystemState() {

    std::lock_guard<std::mutex> lock(mutex_);
    
    Json::Value state;
    Json::Value links(Json::arrayValue);
    
    for (const auto& [link_id, link_state] : link_states_) {
        links.append(link_state->toJson());
    }
    
    state["links"] = links;
    state["timestamp"] = static_cast<Json::UInt64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    
    return state;
}

void CrossLayerStateManager::notifyLinkStateUpdated(int link_id, const LinkState& link_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Called with mutex already locked
    for (auto listener : listeners_) {
        try {
            listener->onLinkStateUpdated(link_id, link_state);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Exception in link state update listener: " << e.what());
        }
    }
    LOG_MODULE_DEBUG(MODULE, "Notified listeners of link state update for " << link_id);
}

void CrossLayerStateManager::notifySystemStateUpdated() {
    // Create a copy of the system state to avoid holding the lock during notification
    Json::Value system_state = getSystemState();

    LOG_MODULE_DEBUG(MODULE, "Notifying listeners of system state update " << system_state);
    
    // Notify listeners
    for (auto listener : listeners_) {
        try {
            listener->onSystemStateUpdated(system_state);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Exception in system state update listener: " << e.what());
        }
    }
    //LOG_MODULE_DEBUG(MODULE, "Notified listeners of system state update");
}

} // namespace ric