#include "scheduler/Single/single_scheduler.hpp"
#include "log.hpp"

const std::string MODULE = "SINGLE_SCHEDULER";

namespace ric {

SingleScheduler::SingleScheduler() 
    : fixed_link_id_(0), 
      initial_decision_made_(false) {
    LOG_MODULE_INFO(MODULE, "SingleScheduler created");
}

SingleScheduler::~SingleScheduler() {
    LOG_MODULE_INFO(MODULE, "SingleScheduler destroyed");
}

bool SingleScheduler::initialize(std::shared_ptr<StateManager> state_manager, 
                                const SchedulerConfig& config) {
    LOG_MODULE_INFO(MODULE, "Initializing SingleScheduler");

    // Call the base class initialize method
    if (!BaseScheduler::initialize(state_manager, config)) {
        return false;
    }
    
    if (!links.empty()) {
        // Use the first link by default; override with SINGLE_LINK_ID env (0=5G, 1=4G).
        fixed_link_id_ = 0;
        const char* e = std::getenv("SINGLE_LINK_ID");
        if (e) {
            try { fixed_link_id_ = std::stoi(e); } catch (...) {}
            LOG_MODULE_ERROR(MODULE, "[SingleScheduler] SINGLE_LINK_ID env='" << e << "' → fixed_link_id_=" << fixed_link_id_);
        } else {
            LOG_MODULE_ERROR(MODULE, "[SingleScheduler] SINGLE_LINK_ID env NOT SET, using fixed_link_id_=" << fixed_link_id_);
        }
    } else {
        LOG_MODULE_WARN(MODULE, "No links available during initialization, will use default: " << fixed_link_id_);
    }
    
    return true;
}

SchedulingDecision SingleScheduler::makeDecisionInternal() {
    SchedulingDecision decision;
    decision.selected_link_id = fixed_link_id_;
    decision.decision_reason = "Single scheduler always uses the same link";
    decision.requires_path_change = !initial_decision_made_; // Only requires change on first decision
    
    if (!initial_decision_made_) {
        initial_decision_made_ = true;
    }

    return decision;
}

void SingleScheduler::onLinkStateUpdated(int link_id, const LinkState& link_state) {
    // Single scheduler does not need to update link since this always selcects the same link
}
}