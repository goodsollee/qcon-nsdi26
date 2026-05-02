#ifndef CU_SCHEDULER_CONTROLLER_HPP
#define CU_SCHEDULER_CONTROLLER_HPP

#include "scheduler_controller_interface.hpp"
#include "RIC/zmq_interface.hpp"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <queue>
#include <condition_variable>
#include <json-c/json.h>
#include <vector>

namespace ric {

/**
 * Command structure for path control
 */
struct PathControlCommand {
    std::string component_id;         // Target component ID
    int path;                         // Path to activate (single-link mode)
    bool is_multilink;                // Whether to use multilink routing
    std::vector<int> link_ids;        // Link IDs for multilink scheduling
    std::vector<double> split_ratios; // Split ratios for multilink scheduling
    
    // Constructor for single path mode
    PathControlCommand(const std::string& id, int p)
        : component_id(id), path(p), is_multilink(false) {}
        
    // Constructor for multilink mode
    PathControlCommand(const std::string& id, const std::vector<int>& links, 
                     const std::vector<double>& ratios)
        : component_id(id), path(0), is_multilink(true), 
          link_ids(links), split_ratios(ratios) {}
};

/**
 * CuSchedulerController - Implementation of the scheduler controller for CU
 * Handles path control for PDCP Sender
 */
class CuSchedulerController : public SchedulerControllerInterface {
public:
    /**
     * Constructor
     * 
     * @param zmq_interface Reference to the ZMQ interface for sending RC commands
     */
    CuSchedulerController(ZmqInterface& zmq_interface);
    
    /**
     * Destructor
     */
    ~CuSchedulerController() override;
    
    /**
     * Initialize the controller
     * 
     * @return true if initialization successful, false otherwise
     */
    bool initialize() override;
    
    /**
     * Start the controller
     * 
     * @return true if started successfully, false otherwise
     */
    bool start() override;
    
    /**
     * Stop the controller
     */
    void stop() override;

    // Add to CuSchedulerController class in the public section:
    /**
    * Send a command to configure packet duplication
    * 
    * @param params JSON parameters for duplication configuration
    * @return true if command sent successfully, false otherwise
    */
    bool sendDuplicationCommand(const Json::Value& params);

    bool sendCommandImmediate(int path);

    /**
     * Send a multilink path RC command synchronously, similar shape to handleSetPath's
     * multilink branch in mc_emulator: { is_multilink: true, link_ids: [...], split_ratios: [...] }
     * link_ids in the input map MUST be 0-indexed (matching mc_emulator path numbering).
     */
    bool sendMultilinkCommandImmediate(const std::map<int, double>& split_ratio_zero_indexed);

    /**
     * Paper §5.4 priority-based re-injection trigger.
     * Tells pdcp_sender to duplicate next `burst_packets` onto `alt_path` with
     * HIGH-priority queue. LCID approximation in our 4-queue emulator config.
     */
    bool sendReinjectBurst(int alt_path, int burst_packets);

    /**
     * Set the active path for a component (single link mode)
     *
     * @param component_id ID of the component (e.g., PDCP Sender)
     * @param path Path index to activate
     * @return true if command queued successfully, false otherwise
     */
    bool setActivePath(const std::string& component_id, int path);
    
    /**
     * Set multilink configuration for a component
     * 
     * @param component_id ID of the component
     * @param link_ids Vector of link IDs to use
     * @param split_ratios Vector of split ratios (must sum to 1.0)
     * @return true if command queued successfully, false otherwise
     */
    bool setMultilinkPath(const std::string& component_id, 
                        const std::vector<int>& link_ids,
                        const std::vector<double>& split_ratios);
    
    /**
     * Get the active path for a component
     * 
     * @param component_id ID of the component
     * @param path Reference to store the active path
     * @return true if component exists and path retrieved, false otherwise
     */
    bool getActivePath(const std::string& component_id, int& path);

    /**
    * Get all registered components
    * 
    * @return Vector of component IDs
    */
    std::vector<std::string> getRegisteredComponents();
    
    /**
     * Register KPM metrics callback for a component
     * 
     * @param component_id ID of the component
     * @param callback Function to call when a metric is received
     */
    void registerKpmCallback(const std::string& component_id, 
                            std::function<void(const std::string&, const std::string&)> callback);
    
private:
    ZmqInterface& zmq_interface_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
    
    std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::queue<PathControlCommand> command_queue_;
    
    std::mutex state_mutex_;
    std::map<std::string, int> active_paths_;
    std::map<std::string, std::function<void(const std::string&, const std::string&)>> kpm_callbacks_;
    
    /**
     * Worker thread function
     */
    void workerThread();
    
    /**
     * Send a path control command to a component
     * 
     * @param component_id ID of the component
     * @param path Path index to activate
     * @return true if command sent successfully, false otherwise
     */
    bool sendPathControlCommand(const std::string& component_id, int path);

    /**
    * Send a multilink path control command to a component
    * 
    * @param component_id ID of the component
    * @param link_ids Vector of link IDs to use
    * @param split_ratios Vector of split ratios
    * @return true if command sent successfully, false otherwise
    */
    bool sendMultilinkPathCommand(const std::string& component_id, 
                                const std::vector<int>& link_ids,
                                const std::vector<double>& split_ratios);
    
    /**
     * Process KPM metrics from a component
     * 
     * @param component_id ID of the component
     * @param metric_type Type of the metric
     * @param metric_value Value of the metric
     */
    void processKpmMetric(const std::string& component_id, 
                         const std::string& metric_type, 
                         const std::string& metric_value);
    
    /**
     * Parse path statistics from a KPM metric
     * 
     * @param metric_value JSON string containing path statistics
     * @param active_path Reference to store the current active path
     * @return true if parsing successful, false otherwise
     */
    bool parsePathStats(const std::string& metric_value, int& active_path);
};

} // namespace ric

#endif // CU_SCHEDULER_CONTROLLER_HPP