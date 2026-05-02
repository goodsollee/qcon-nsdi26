#ifndef PACKET_DUPLICATION_SCHEDULER_HPP
#define PACKET_DUPLICATION_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include "state_manager/link_state.hpp"
#include <vector>
#include <string>

namespace ric {

/**
 * PacketDuplicationScheduler - A scheduler that simply enables packet duplication
 * on all available links.
 */
class PacketDuplicationScheduler : public BaseScheduler {
public:
    PacketDuplicationScheduler();
    ~PacketDuplicationScheduler() override;
    
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                   const SchedulerConfig& config) override;
    
protected:
    SchedulingDecision makeDecisionInternal() override;
    bool executeDecision(const SchedulingDecision& decision) override;
    
    // Override to handle system state updates
    void onSystemStateUpdated(const Json::Value& system_state) override;
};

} // namespace ric

#endif // PACKET_DUPLICATION_SCHEDULER_HPP