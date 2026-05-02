#ifndef DU_SCHEDULER_CONTROLLER_HPP
#define DU_SCHEDULER_CONTROLLER_HPP

#include "scheduler_controller_interface.hpp"
#include "RIC/zmq_interface.hpp"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <queue>

namespace ric {

/**
 * DuSchedulerController - Implementation of the scheduler controller for DU
 * Handles controlling DU components like PHY sender
 */
class DuSchedulerController : public SchedulerControllerInterface {
public:
    /**
     * Constructor
     * 
     * @param zmq_interface Reference to the ZMQ interface for sending RC commands
     */
    DuSchedulerController(ZmqInterface& zmq_interface);
    
    /**
     * Destructor
     */
    ~DuSchedulerController() override;
    
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
    
    /**
     * Send start trace command to PHY sender
     * 
     * @param component_id ID of the component (e.g., PHY Sender)
     * @return true if command sent successfully, false otherwise
     */
    bool sendStartTraceCommand(const std::string& component_id);
    
private:
    ZmqInterface& zmq_interface_;
    std::atomic<bool> running_;
    
    /**
     * Send a command to a DU component
     * 
     * @param component_id ID of the component
     * @param command_type Type of command
     * @param params Command parameters as JSON string
     * @return true if command sent successfully, false otherwise
     */
    bool sendCommand(const std::string& component_id, 
                    const std::string& command_type,
                    const std::string& params);
};

} // namespace ric

#endif // DU_SCHEDULER_CONTROLLER_HPP