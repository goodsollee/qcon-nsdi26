#ifndef SINGLE_SCHEDULER_HPP
#define SINGLE_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include <vector>

namespace ric {

/**
 * SingleScheduler - A simple scheduler that always uses a single link
 */
class SingleScheduler : public BaseScheduler {
public:
    SingleScheduler();
    ~SingleScheduler() override;
    
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                   const SchedulerConfig& config) override;
    
    // Override from StateUpdateListener
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;
    
protected:
    SchedulingDecision makeDecisionInternal() override;
    
private:
    int fixed_link_id_;
    bool initial_decision_made_;
    std::vector<std::string> links;
};

} // namespace ric

#endif // SINGLE_SCHEDULER_HPP