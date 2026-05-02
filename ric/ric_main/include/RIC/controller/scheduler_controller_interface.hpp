#ifndef SCHEDULER_CONTROLLER_INTERFACE_HPP
#define SCHEDULER_CONTROLLER_INTERFACE_HPP

#include <string>
#include <functional>
#include <memory>

namespace ric {

// Forward declarations
class ZmqInterface;

/**
 * SchedulerControllerInterface - Abstract interface for scheduler controllers
 * This class defines the interface for both CU and DU scheduler controllers
 */
class SchedulerControllerInterface {
public:
    virtual ~SchedulerControllerInterface() = default;
    
    /**
     * Initialize the controller
     * 
     * @return true if initialization successful, false otherwise
     */
    virtual bool initialize() = 0;
    
    /**
     * Start the controller
     * 
     * @return true if started successfully, false otherwise
     */
    virtual bool start() = 0;
    
    /**
     * Stop the controller
     */
    virtual void stop() = 0;
};

/**
 * SchedulerControllerFactory - Factory for creating scheduler controllers
 */
class SchedulerControllerFactory {
public:
    /**
     * Create a CU scheduler controller
     * 
     * @param zmq_interface Reference to the ZMQ interface for sending RC commands
     * @return Unique pointer to the controller
     */
    static std::unique_ptr<SchedulerControllerInterface> createCuSchedulerController(
        ZmqInterface& zmq_interface);
    
    /**
     * Create a DU scheduler controller
     * 
     * @param zmq_interface Reference to the ZMQ interface for sending RC commands
     * @return Unique pointer to the controller
     */
    static std::unique_ptr<SchedulerControllerInterface> createDuSchedulerController(
        ZmqInterface& zmq_interface);
};

} // namespace ric

#endif // SCHEDULER_CONTROLLER_INTERFACE_HPP