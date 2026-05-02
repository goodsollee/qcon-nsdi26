#pragma once

#include "RIC/kpm_processor.hpp"
#include "state_manager/state_manager.hpp"
#include <memory>

namespace ric {
    class KpmProcessor;
    class StateManager;  // If you need to hold a pointer or shared_ptr of StateManager


/**
 * KpmStateIntegration - Class to integrate KPM Processor with State Manager
 * This acts as a bridge between the KPM metrics and the state manager
 */
class KpmStateIntegration {
public:
    /**
     * Constructor
     * 
     * @param kpm_processor Reference to the KPM processor
     * @param state_manager Reference to the state manager
     */
    KpmStateIntegration(std::shared_ptr<KpmProcessor> kpm_processor, std::shared_ptr<StateManager> state_manager);
    
    /**
     * Destructor
     */
    ~KpmStateIntegration();
    
    /**
     * Initialize the integration
     * 
     * @return true if initialization was successful, false otherwise
     */
    bool initialize();
    
private:
    std::shared_ptr<KpmProcessor> kpm_processor_;
    std::shared_ptr<StateManager> state_manager_;
    
    // Component mapping
    struct ComponentInfo {
        std::string component_id;
        std::string component_type;
    };
    
    std::vector<ComponentInfo> registered_components_;
    
    // Callback for RLC performance metrics
    void handleRlcPerformanceMetrics(const std::string& component_id, 
                                    const std::string& component_type,
                                    const std::string& metric_value);
    
    // Callback for PDCP path statistics
    void handlePdcpPathStats(const std::string& component_id, 
                            const std::string& component_type,
                            const std::string& metric_value);
};

} // namespace ric
