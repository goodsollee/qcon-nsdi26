#include "scheduler/minRTT/minrtt_scheduler.hpp"
#include "log.hpp"
#include <algorithm>
#include <limits>

const std::string MODULE = "MINRTT_SCHEDULER";

namespace ric {

MinRttScheduler::MinRttScheduler()
    : initial_decision_made_(false) {
    LOG_MODULE_INFO(MODULE, "MinRttScheduler created");
}

MinRttScheduler::~MinRttScheduler() {
    LOG_MODULE_INFO(MODULE, "MinRttScheduler destroyed");
}

bool MinRttScheduler::initialize(std::shared_ptr<StateManager> state_manager,
                               const SchedulerConfig& config) {

    // Call the base class initialize method
    if (!BaseScheduler::initialize(state_manager, config)) {
        return false;
    }
    
    LOG_MODULE_INFO(MODULE, "Initializing MinRttScheduler");
    return true;
}

SchedulingDecision MinRttScheduler::makeDecisionInternal() {
    SchedulingDecision decision;
    
    // Select the link with minimum RTT
    int selected_link_id = selectLinkWithMinRtt();
    
    decision.selected_link_id = selected_link_id;
    decision.decision_reason = "Selected link with minimum RTT";
    
    // Determine if path change is required
    bool requires_path_change = !initial_decision_made_ || (current_link_id_ != selected_link_id);
    decision.requires_path_change = requires_path_change;
    
    if (requires_path_change) {
        LOG_MODULE_INFO(MODULE, "Switching to link " << selected_link_id << " with lower RTT");
    }
    
    if (!initial_decision_made_) {
        initial_decision_made_ = true;
    }
    
    return decision;
}

void MinRttScheduler::onLinkStateUpdated(int link_id, const LinkState& link_state) {
    // Nothing to do here for MinRttScheduler
}

void MinRttScheduler::updateLinkMetrics(const std::map<int, LinkMetrics>& metrics) {
    
    // Update our local link delay tracking
    for (const auto& pair : metrics) {
        int link_id = pair.first;
        const LinkMetrics& link_metrics = pair.second;
        
        link_delays_[link_id] = link_metrics.delay_ms;
        LOG_MODULE_DEBUG(MODULE, "Updated RTT for link " << link_id << " to " << link_metrics.delay_ms << " ms");
    }
    BaseScheduler::runOnce();
}

int MinRttScheduler::selectLinkWithMinRtt() const {
    
    int selected_link_id = 0; // Default in case no links available
    double min_delay = std::numeric_limits<double>::max();
    
    if (link_delays_.empty()) {
        LOG_MODULE_WARN(MODULE, "No link delay information available, using default link " << selected_link_id);
        return selected_link_id;
    }
    
    // Find the link with minimum delay
    for (const auto& pair : link_delays_) {
        int link_id = pair.first;
        double delay = pair.second;
        
        if (delay < min_delay) {
            min_delay = delay;
            selected_link_id = link_id - 1;
        }
    }
    
    LOG_MODULE_DEBUG(MODULE, "Selected link " << selected_link_id << " with minimum RTT of " << min_delay << " ms");
    return selected_link_id;
}

} // namespace ric