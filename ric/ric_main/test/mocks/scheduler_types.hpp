#pragma once

#include <string>
#include <map>
#include <vector>
#include <jsoncpp/json/json.h>

namespace ric {

// Scheduling decision structure
struct SchedulingDecision {
    std::string decision_type;
    std::map<std::string, std::string> parameters;
    std::map<int, double> link_proportions;
    std::map<int, bool> forwarding_enabled;
    
    Json::Value toJson() const {
        Json::Value root;
        root["decision_type"] = decision_type;
        
        // Parameters
        Json::Value params;
        for (const auto& [key, value] : parameters) {
            params[key] = value;
        }
        root["parameters"] = params;
        
        // Link proportions
        Json::Value links;
        for (const auto& [link_id, proportion] : link_proportions) {
            links[std::to_string(link_id)] = proportion;
        }
        root["link_proportions"] = links;
        
        // Forwarding
        Json::Value forwarding;
        for (const auto& [link_id, enabled] : forwarding_enabled) {
            forwarding[std::to_string(link_id)] = enabled;
        }
        root["forwarding_enabled"] = forwarding;
        
        return root;
    }
};

// Scheduler configuration
struct SchedulerConfig {
    std::string scheduler_name = "qcon";
    bool enable_logging = true;
    bool is_enabled = true;
    std::string algorithm = "default";
    double dchannel_alpha = 0.75;
};

} // namespace ric