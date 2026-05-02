#include "scheduler/PD/duplication_scheduler.hpp"
#include "RIC/controller/cu_scheduler_controller.hpp"
#include "log.hpp"
#include <jsoncpp/json/json.h>

const std::string MODULE = "PACKET_DUPLICATION_SCHEDULER";

namespace ric {

PacketDuplicationScheduler::PacketDuplicationScheduler() {
    LOG_MODULE_INFO(MODULE, "PacketDuplicationScheduler created");
}

PacketDuplicationScheduler::~PacketDuplicationScheduler() {
    LOG_MODULE_INFO(MODULE, "PacketDuplicationScheduler destroyed");
}

bool PacketDuplicationScheduler::initialize(std::shared_ptr<StateManager> state_manager,
                                         const SchedulerConfig& config) {
    // Call the base class initialize method
    if (!BaseScheduler::initialize(state_manager, config)) {
        return false;
    }
    
    LOG_MODULE_INFO(MODULE, "Initializing PacketDuplicationScheduler");
    return true;
}

SchedulingDecision PacketDuplicationScheduler::makeDecisionInternal() {
    SchedulingDecision decision;
    
    // Get link states from state manager
    std::vector<std::shared_ptr<LinkState>> link_states;
    if (state_manager_) {
        link_states = state_manager_->getActiveLinkStates();
    }
    
    // Use the first link as the selected link ID if available
    if (!link_states.empty()) {
        decision.selected_link_id = link_states[0]->getLinkId();
    } else {
        decision.selected_link_id = 0; // Default if no links available
    }
    
    // Simple reason - we're enabling packet duplication
    decision.decision_reason = "Enabling packet duplication";
    
    // Always indicate that a path change is needed
    // This ensures the executeDecision method is called each time
    decision.requires_path_change = true;
    
    return decision;
}

bool PacketDuplicationScheduler::executeDecision(const SchedulingDecision& decision) {
    if (!controller_) {
        LOG_MODULE_ERROR(MODULE, "Cannot execute decision: no controller set");
        return false;
    }
    
    // Get link states from state manager
    std::vector<std::shared_ptr<LinkState>> link_states;
    if (state_manager_) {
        link_states = state_manager_->getActiveLinkStates();
    }
    
    // Extract link IDs for duplication
    std::vector<int> duplication_paths;
    for (const auto& link : link_states) {
        duplication_paths.push_back(link->getLinkId());
    }
    
    // Create duplication configuration
    Json::Value duplication_params;
    duplication_params["enabled"] = true;  // Always enable duplication
    
    // Add paths array
    Json::Value paths_array(Json::arrayValue);
    for (int path : duplication_paths) {
        paths_array.append(path);
    }
    duplication_params["paths"] = paths_array;
    
    // Create debug string of paths
    std::string paths_str;
    for (size_t i = 0; i < duplication_paths.size(); i++) {
        if (i > 0) paths_str += ", ";
        paths_str += std::to_string(duplication_paths[i]);
    }
    
    // Send duplication command
    bool duplication_success = controller_->sendDuplicationCommand(duplication_params);
    
    if (!duplication_success) {
        LOG_MODULE_ERROR(MODULE, "Failed to enable packet duplication");
        return false;
    }
    
    // Update the current link ID 
    std::lock_guard<std::mutex> lock(mutex_);
    current_link_id_ = decision.selected_link_id;
    LOG_MODULE_INFO(MODULE, "Successfully enabled packet duplication on paths [" << paths_str << "]");
    
    return true;
}

void PacketDuplicationScheduler::onSystemStateUpdated(const Json::Value& system_state) {
    BaseScheduler::onSystemStateUpdated(system_state);
}

} // namespace ric