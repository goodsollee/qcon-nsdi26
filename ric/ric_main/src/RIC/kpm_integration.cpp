#include "RIC/kpm_integration.hpp"
#include "log.hpp"

using namespace std;
const string KPM_MODULE = "KPM_INTEGRATION";

namespace ric {

KpmStateIntegration::KpmStateIntegration(std::shared_ptr<KpmProcessor> kpm_processor,
                                         std::shared_ptr<StateManager> state_manager)
    : kpm_processor_(kpm_processor),
      state_manager_(state_manager) {
    LOG_MODULE_INFO(KPM_MODULE, "KpmStateIntegration created");
}

KpmStateIntegration::~KpmStateIntegration() {
    LOG_MODULE_INFO(KPM_MODULE, "KpmStateIntegration destroyed");
}

bool KpmStateIntegration::initialize() {
    LOG_MODULE_INFO(KPM_MODULE, "Initializing KPM to State Manager integration");
    
    // Register for specific metrics we care about
    // Note: This assumes modifications to KpmProcessor to support these callbacks
    
    // For DU RLC metrics
    kpm_processor_->registerMetricCallback("rlc_performance_metrics", 
        [this](const std::string& component_id, const std::string& metric_type, const std::string& metric_value) {
            // Determine component type (assume DU for RLC metrics)
            this->handleRlcPerformanceMetrics(component_id, "DU", metric_value);
        }
    );
    
    // For PDCP path statistics
    kpm_processor_->registerMetricCallback("pdcp_path_stats", 
        [this](const std::string& component_id, const std::string& metric_type, const std::string& metric_value) {
            // Determine component type (assume CU for PDCP metrics)
            this->handlePdcpPathStats(component_id, "CU", metric_value);
        }
    );
    
    LOG_MODULE_INFO(KPM_MODULE, "KPM integration initialized");
    return true;
}

void KpmStateIntegration::handleRlcPerformanceMetrics(const std::string& component_id, 
                                                    const std::string& component_type,
                                                    const std::string& metric_value) {
    LOG_MODULE_DEBUG(KPM_MODULE, "Forwarding RLC metrics from " << component_id << " to state manager");
    
    // Register component if not already known
    bool known = false;
    for (const auto& info : registered_components_) {
        if (info.component_id == component_id) {
            known = true;
            break;
        }
    }
    
    if (!known) {
        LOG_MODULE_INFO(KPM_MODULE, "Registering new component: " << component_id << " (type: " << component_type << ")");
        registered_components_.push_back({component_id, component_type});
    }
    
    // Forward to state manager
    state_manager_->processMetrics(component_id, component_type, "rlc_performance_metrics", metric_value);
}

void KpmStateIntegration::handlePdcpPathStats(const std::string& component_id, 
                                            const std::string& component_type,
                                            const std::string& metric_value) {
    LOG_MODULE_DEBUG(KPM_MODULE, "Forwarding PDCP path stats from " << component_id << " to state manager");
    
    // Register component if not already known
    bool known = false;
    for (const auto& info : registered_components_) {
        if (info.component_id == component_id) {
            known = true;
            break;
        }
    }
    
    if (!known) {
        LOG_MODULE_INFO(KPM_MODULE, "Registering new component: " << component_id << " (type: " << component_type << ")");
        registered_components_.push_back({component_id, component_type});
    }
    
    // Forward to state manager
    state_manager_->processMetrics(component_id, component_type, "pdcp_path_stats", metric_value);
}

} // namespace ric