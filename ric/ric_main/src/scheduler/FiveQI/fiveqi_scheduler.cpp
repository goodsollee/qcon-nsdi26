#include "scheduler/FiveQI/fiveqi_scheduler.hpp"
#include "log.hpp"
#include <cstdlib>

const std::string MODULE = "FIVEQI_SCHEDULER";

namespace ric {

FiveQiScheduler::FiveQiScheduler() : slo_ms_(100.0), last_pick_(0) {
    if (const char* e = std::getenv("QCON_5QI_SLO_MS")) {
        try { slo_ms_ = std::stod(e); } catch (...) {}
    }
    LOG_MODULE_INFO(MODULE, "FiveQiScheduler created, SLO=" << slo_ms_ << " ms");
}

FiveQiScheduler::~FiveQiScheduler() {
    LOG_MODULE_INFO(MODULE, "FiveQiScheduler destroyed");
}

bool FiveQiScheduler::initialize(std::shared_ptr<StateManager> state_manager,
                                 const SchedulerConfig& config) {
    return BaseScheduler::initialize(state_manager, config);
}

void FiveQiScheduler::onLinkStateUpdated(int /*link_id*/, const LinkState& /*link_state*/) {}

SchedulingDecision FiveQiScheduler::makeDecisionInternal() {
    // Estimate per-frame size from delivered byte rate; use 50 KB fallback.
    // Without QoE plumbing, the heuristic is bandwidth-only: if measured 5G
    // throughput is large enough that 50KB / 5G_bw < SLO, stay on 5G; else 4G.
    SchedulingDecision decision;
    decision.selected_link_id = 0;
    decision.decision_reason  = "5QI default to 5G";
    decision.requires_path_change = false;
    decision.split_ratio = { { 0, 1.0 } };

    constexpr double FRAME_KB = 50.0;
    double bw5g = 0.0, bw4g = 0.0;
    if (state_manager_) {
        auto ls5g = state_manager_->getLinkState(1);
        auto ls4g = state_manager_->getLinkState(2);
        if (ls5g) bw5g = ls5g->getThroughputMbps();
        if (ls4g) bw4g = ls4g->getThroughputMbps();
    }
    // ECT(ms) = frame_kb * 8 / bw_mbps
    double ect_5g = (bw5g > 0) ? (FRAME_KB * 8.0 / bw5g) : 1e9;
    double ect_4g = (bw4g > 0) ? (FRAME_KB * 8.0 / bw4g) : 1e9;

    int pick = 0;
    if (ect_5g <= slo_ms_) {
        pick = 0;
        decision.decision_reason = "5QI: 5G meets SLO (" + std::to_string((int)ect_5g) + "ms)";
    } else if (ect_4g < ect_5g) {
        pick = 1;
        decision.decision_reason = "5QI: 5G ECT (" + std::to_string((int)ect_5g) +
                                   "ms) > SLO, switch to 4G (" + std::to_string((int)ect_4g) + "ms)";
    } else {
        pick = 0;
        decision.decision_reason = "5QI: both above SLO, stay 5G";
    }
    decision.selected_link_id = pick;
    decision.requires_path_change = (pick != last_pick_);
    decision.split_ratio = { { pick, 1.0 } };
    last_pick_ = pick;
    return decision;
}

}  // namespace ric
