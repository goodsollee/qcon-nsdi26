#include "scheduler_controller_interface.hpp"
#include "cu_scheduler_controller.hpp"
#include "du_scheduler_controller.hpp"

namespace ric {

std::unique_ptr<SchedulerControllerInterface> SchedulerControllerFactory::createCuSchedulerController(
    ZmqInterface& zmq_interface) {
    return std::make_unique<CuSchedulerController>(zmq_interface);
}

std::unique_ptr<SchedulerControllerInterface> SchedulerControllerFactory::createDuSchedulerController(
    ZmqInterface& zmq_interface) {
    return std::make_unique<DuSchedulerController>(zmq_interface);
}

} // namespace ric