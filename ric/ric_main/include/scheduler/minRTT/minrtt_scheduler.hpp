#ifndef MINRTT_SCHEDULER_HPP
#define MINRTT_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include <vector>
#include <map>
#include <string>

namespace ric {

/**
 * MinRttScheduler - A scheduler that selects the link with the lowest round-trip time (RTT)
 */
class MinRttScheduler : public BaseScheduler {
public:
    MinRttScheduler();
    ~MinRttScheduler() override;
    
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                   const SchedulerConfig& config) override;
    
    // Override from StateUpdateListener
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;
    
protected:
    SchedulingDecision makeDecisionInternal() override;
    
    // Override to update the link metrics when system state changes
    void updateLinkMetrics(const std::map<int, LinkMetrics>& metrics) override;
    
private:
    std::map<int, double> link_delays_; // Map of link IDs to their RTT values
    bool initial_decision_made_;
    
    // Helper method to select the link with minimum RTT
    int selectLinkWithMinRtt() const;
};

} // namespace ric

#endif // MINRTT_SCHEDULER_HPP