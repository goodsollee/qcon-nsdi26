#ifndef STATE_MANAGER_HPP
#define STATE_MANAGER_HPP

#include "state_manager/link_state.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <jsoncpp/json/json.h>
#include <set>

namespace ric {

/**
 * StateUpdateListener - Interface for components that want to be notified of state changes
 */
class StateUpdateListener {
public:
    virtual ~StateUpdateListener() = default;
    
    /**
     * Called when a link state is updated
     * 
     * @param link_id ID of the updated link
     * @param link_state Reference to the updated link state
     */
    virtual void onLinkStateUpdated(int link_id, const LinkState& link_state) = 0;
    
    /**
     * Called when overall system state is updated
     * 
     * @param system_state JSON representation of the complete system state
     */
    virtual void onSystemStateUpdated(const Json::Value& system_state) = 0;
};

/**
 * StateManager - Interface for managing system state
 */
class StateManager {
public:
    virtual ~StateManager() = default;
    
    /**
     * Initialize the state manager
     * 
     * @return true if initialization was successful, false otherwise
     */
    virtual bool initialize() = 0;
    
    /**
     * Process metrics from a RAN component
     * 
     * @param component_id ID of the component that sent the metrics
     * @param component_type Type of the component (e.g., "DU", "CU")
     * @param metric_type Type of the metric (e.g., "rlc_performance_metrics")
     * @param metric_value JSON string containing the metric data
     * @return true if processing was successful, false otherwise
     */
    virtual bool processMetrics(const std::string& component_id,
                              const std::string& component_type,
                              const std::string& metric_type,
                              const std::string& metric_value) = 0;
    
    /**
     * Get a link state by ID
     * 
     * @param link_id ID of the link
     * @return Shared pointer to the link state, or nullptr if not found
     */
    virtual std::shared_ptr<LinkState> getLinkState(int link_id) = 0;
    
    /**
     * Get all active link states
     * 
     * @return Vector of shared pointers to active link states
     */
    virtual std::vector<std::shared_ptr<LinkState>> getActiveLinkStates() = 0;
    
    /**
     * Register a listener for state updates
     * 
     * @param listener Pointer to the listener
     */
    virtual void registerListener(StateUpdateListener* listener) = 0;
    
    /**
     * Unregister a listener
     * 
     * @param listener Pointer to the listener to unregister
     */
    virtual void unregisterListener(StateUpdateListener* listener) = 0;
    
    /**
     * Get complete system state as JSON
     * 
     * @return JSON representation of the complete system state
     */
    virtual Json::Value getSystemState() = 0;
};

/**
 * CrossLayerStateManager - Implementation of StateManager
 */
class CrossLayerStateManager : public StateManager {
public:
    CrossLayerStateManager();
    ~CrossLayerStateManager();
    
    bool initialize() override;
    
    bool processMetrics(const std::string& component_id,
                      const std::string& component_type,
                      const std::string& metric_type,
                      const std::string& metric_value) override;
    
    std::shared_ptr<LinkState> getLinkState(int link_id) override;
    
    std::vector<std::shared_ptr<LinkState>> getActiveLinkStates() override;

    void registerListener(StateUpdateListener* listener) override;
    
    void unregisterListener(StateUpdateListener* listener) override;
    
    Json::Value getSystemState() override;
    
private:
    // Map of link states by link ID
    std::map<int, std::shared_ptr<LinkState>> link_states_;
    
    // Map to translate component IDs to link IDs
    std::map<std::string, int> component_to_link_map_;
    
    // Set of component IDs to help generate unique IDs
    std::set<std::string> component_ids_;
    
    // Registered listeners
    std::vector<StateUpdateListener*> listeners_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Helper methods
    void notifyLinkStateUpdated(int link_id, const LinkState& link_state);
    void notifySystemStateUpdated();
    void processRlcPerformanceMetrics(int link_id, const Json::Value& metrics);
    void processPdcpPerformanceMetrics(const Json::Value& metrics) ;
    
    // Determine link ID from component ID and type
    int getLinkIdFromComponent(const std::string& component_id, const std::string& component_type);
};

} // namespace ric

#endif // STATE_MANAGER_HPP