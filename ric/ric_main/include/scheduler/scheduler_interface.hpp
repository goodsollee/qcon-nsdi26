#ifndef SCHEDULER_INTERFACE_HPP
#define SCHEDULER_INTERFACE_HPP

#include "state_manager/state_manager.hpp"
// Remove this line: #include "scheduler/Single/single_scheduler.hpp"
#include <string>
#include <memory>
#include <functional>
#include <jsoncpp/json/json.h>
#include <thread>
#include <atomic>

namespace ric {

// Forward declarations
class CuSchedulerController;
class SingleScheduler; // Forward declare instead of including

/**
 * SchedulerConfig - Configuration for a scheduler
 */
struct SchedulerConfig {
    // Enable/disable the scheduler
    bool is_enabled = true;         // Whether the scheduler should be used
    
    // General settings
    std::string scheduler_name;      // Name of the scheduler
    bool enable_logging = true;      // Enable logging of decisions
    
    // Decision parameters
    double throughput_weight = 0.5;  // Weight for throughput in decision
    double latency_weight = 0.3;     // Weight for latency in decision
    double stability_weight = 0.2;   // Weight for stability (avoiding frequent switches)
    
    // Hysteresis parameters
    double link_switch_threshold = 0.2;  // Minimum improvement to switch links
    int min_switch_interval_ms = 1000;   // Minimum time between switches

    double dchannel_alpha = 0.75; // Alpha value for DChannel scheduler
};

/**
 * SchedulingDecision - Result of a scheduling decision
 */
struct SchedulingDecision {
    int selected_link_id = 0;     // Default to link 0 — uninitialized value caused
                                  // pdcp_sender crash ("Invalid path value: 134217728").
    std::map<int, double> split_ratio;
    std::string decision_reason;
    bool requires_path_change = false;
    bool is_multipath = false;
};

/**
 * SchedulerInterface - Interface for scheduler implementations
 */
class SchedulerInterface : public StateUpdateListener {
public:
    virtual ~SchedulerInterface() = default;

    /**
     * Initialize the scheduler
     * 
     * @param state_manager Pointer to the state manager
     * @param config Configuration for the scheduler
     * @return true if initialization was successful, false otherwise
     */
    virtual bool initialize(std::shared_ptr<StateManager> state_manager, 
                          const SchedulerConfig& config) = 0;
    
    /**
     * Set the controller for executing commands
     * 
     * @param controller Shared pointer to the CU scheduler controller
     */
    virtual void setController(std::shared_ptr<CuSchedulerController> controller) = 0;
    
    /**
     * Make a scheduling decision
     * 
     * @return The scheduling decision
     */
    virtual SchedulingDecision makeDecision() = 0;
    
    /**
     * Execute a scheduling decision (e.g., send commands to change path)
     * 
     * @param decision The decision to execute
     * @return true if execution was successful, false otherwise
     */
    virtual bool executeDecision(const SchedulingDecision& decision) = 0;
    
    /**
     * Run the scheduler once (make and execute a decision)
     * 
     * @return true if successful, false otherwise
     */
    virtual bool runOnce() = 0;
    
    /**
     * Start the scheduler in a continuous loop
     * 
     * @param interval_ms Interval between decisions in milliseconds
     * @return true if started successfully, false otherwise
     */
    virtual bool start(int interval_ms = 1000) = 0;
    
    /**
     * Stop the scheduler
     */
    virtual void stop() = 0;
    
    /**
     * Set a callback for path change events
     * 
     * @param callback Function to call when path is changed
     */
    virtual void setPathChangeCallback(
        std::function<void(const std::string&, int)> callback) = 0;
    
    /**
     * Get the currently selected link ID
     * 
     * @return ID of the currently selected link
     */
    virtual int getCurrentLinkId() const = 0;
    
    /**
     * Get the scheduler configuration
     * 
     * @return Current scheduler configuration
     */
    virtual SchedulerConfig getConfig() const = 0;
    
    /**
     * Update scheduler configuration
     * 
     * @param config New configuration
     */
    virtual void updateConfig(const SchedulerConfig& config) = 0;
    
    /**
     * Get scheduler statistics
     * 
     * @return JSON with scheduler statistics
     */
    virtual Json::Value getStatistics() const = 0;
    
    // StateUpdateListener interface implementation
    virtual void onLinkStateUpdated(int link_id, const LinkState& link_state) override = 0;
    virtual void onSystemStateUpdated(const Json::Value& system_state) override = 0;
};

/**
 * BaseScheduler - Base implementation of SchedulerInterface with common functionality
 */
class BaseScheduler : public SchedulerInterface {
public:
    BaseScheduler();
    virtual ~BaseScheduler();
    
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                   const SchedulerConfig& config) override;
    
    void setController(std::shared_ptr<CuSchedulerController> controller) override;
    
    SchedulingDecision makeDecision() override;
    
    bool executeDecision(const SchedulingDecision& decision) override;
    
    bool runOnce() override;
    
    bool start(int interval_ms = 1000) override;
    
    void stop() override;
    
    void setPathChangeCallback(std::function<void(const std::string&, int)> callback) override;
    
    int getCurrentLinkId() const override;
    
    SchedulerConfig getConfig() const override;
    
    void updateConfig(const SchedulerConfig& config) override;
    
    Json::Value getStatistics() const override;
    
    // StateUpdateListener interface implementation
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;
    void onSystemStateUpdated(const Json::Value& system_state) override;
    
protected:
    // Protected members for derived classes
    std::shared_ptr<StateManager> state_manager_;
    SchedulerConfig config_;
    int current_link_id_;
    SchedulingDecision current_decision_;
    std::chrono::steady_clock::time_point last_switch_time_;
    
    // Path change callback
    std::function<void(const std::string&, int)> path_change_callback_;
    
    // Controller for sending commands
    std::shared_ptr<CuSchedulerController> controller_;
    
    // Thread for continuous scheduling
    std::thread scheduler_thread_;
    std::atomic<bool> running_;
    std::mutex thread_mutex_;
    
    // Statistics
    struct {
        int total_decisions = 0;
        int path_changes = 0;
        int hysteresis_prevented_changes = 0;
        std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    } stats_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Virtual method for scheduler-specific decision logic
    virtual SchedulingDecision makeDecisionInternal() = 0;
    
    // Helper methods
    bool shouldSwitchLinks(const std::string& candidate_link_id, double candidate_score, double current_score);
    int extractPathFromLinkId(const std::string& link_id);
    
    // Thread function for continuous scheduling
    void schedulerThreadFunc(int interval_ms);

    // Structure to hold link-specific metrics
    struct LinkMetrics {
        double bandwidth_mbps = 0.0;
        double delay_ms = 0.0;
        double loss_percent = 0.0;
        uint64_t queue_size_bytes = 0;
        uint64_t acked_bytes = 0;
        uint64_t pdcp_transmitted_bytes = 0; // Total PDCP transmitted bytes
    };
    
    // Map of link IDs to their current metrics
    std::map<int, LinkMetrics> link_metrics_;
    
    // Update methods for derived classes to override
    virtual void updateLinkMetrics(const std::map<int, LinkMetrics>& metrics) {
        std::lock_guard<std::mutex> lock(mutex_);
        link_metrics_ = metrics;
    }
    
    // Template method for derived classes to react to metric updates
    virtual void onSystemMetricsUpdated(double bandwidth_mbps, double delay_ms, double buffer_occupancy_percent) {
        // Base implementation does nothing
        // Derived classes should override this to implement specific reactions
    }
};

/**
 * Factory for creating schedulers
 */
class SchedulerFactory {
public:
    /**
     * Create a scheduler by name
     * 
     * @param name Name of the scheduler to create
     * @param state_manager Pointer to the state manager
     * @param config Configuration for the scheduler
     * @param controller Pointer to the CU scheduler controller
     * @return Shared pointer to the created scheduler, or nullptr if creation failed
     */
    static std::shared_ptr<SchedulerInterface> createScheduler(
        const std::string& name,
        std::shared_ptr<StateManager> state_manager,
        const SchedulerConfig& config,
        std::shared_ptr<CuSchedulerController> controller);
};

} // namespace ric

#endif // SCHEDULER_INTERFACE_HPP