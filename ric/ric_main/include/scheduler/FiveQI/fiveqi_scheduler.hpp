#ifndef FIVEQI_SCHEDULER_HPP
#define FIVEQI_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include <vector>

namespace ric {

/**
 * FiveQiScheduler — paper baseline §7 "5QI"
 *
 * Modified 5G QoS Indicator with a 100 ms packet-latency SLO.
 * Default path = link 0 (5G). Switch to link 1 (4G) only when
 * 5G's predicted delivery time exceeds the SLO. Once recovered,
 * fall back. No multilink scheduling, no re-injection.
 *
 * Behaviour heuristic in our setup:
 *   measured 5G_BW (KPM) ≥ frame_size / SLO  → link 0
 *   else                                     → link 1
 *
 * SLO override via QCON_5QI_SLO_MS env (default 100).
 */
class FiveQiScheduler : public BaseScheduler {
public:
    FiveQiScheduler();
    ~FiveQiScheduler() override;

    bool initialize(std::shared_ptr<StateManager> state_manager,
                    const SchedulerConfig& config) override;

    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;

protected:
    SchedulingDecision makeDecisionInternal() override;

private:
    double slo_ms_;
    int    last_pick_;
};

}  // namespace ric

#endif  // FIVEQI_SCHEDULER_HPP
