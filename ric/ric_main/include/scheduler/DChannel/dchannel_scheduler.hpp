#ifndef DCHANNEL_SCHEDULER_HPP
#define DCHANNEL_SCHEDULER_HPP

#include "scheduler/scheduler_interface.hpp"
#include "state_manager/link_state.hpp"
#include <map>
#include <string>
#include <memory>

namespace ric {

/**
 * A simple struct to hold link metrics we care about, derived from LinkState.
 */
struct DChannelLinkInfo {
    double min_rtt_ms;       // e.g. from getAverageDelayMs() or getLatestDelayMs()
    double throughput_mbps;  // from getThroughputMbps()
    double buffer_bytes;     // from getBufferOccupancyBytes()
};

/**
 * DChannelScheduler:
 *   - Demonstrates how to implement per-packet "Algorithm 1" logic:
 *       c_llc, c_hbc, rewards, cost, then comparing (rewards > alpha*cost).
 *   - We assume each packet is exactly 1400 bytes.
 *   - We store minimal state for each link (in link_info_).
 */
class DChannelScheduler : public BaseScheduler {
public:
    DChannelScheduler();
    ~DChannelScheduler() override;

    bool initialize(std::shared_ptr<StateManager> state_manager,
                    const SchedulerConfig& config) override;

    // Called by your data-plane code (or TUN/proxy) for each outgoing packet:
    // Returns the link_id to use (e.g. 0 for eMBB, 1 for URLLC).
    int decideLinkForPacket();

protected:
    // The base class expects us to provide this, but for a per-packet solution
    // we typically do not rely on a single "makeDecisionInternal" for all traffic.
    SchedulingDecision makeDecisionInternal() override;

    // Called when the framework detects a link state update.
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override;

    // Called by the framework (BaseScheduler) to push link metrics. We'll store them.
    void updateLinkMetrics(const std::map<int, LinkMetrics>& metrics) override;

private:
    // For simplicity, assume link 0 = HBC, link 1 = LLC. 
    // If you have more links, adapt accordingly.
    std::map<int, DChannelLinkInfo> link_info_;

    // We track a "completion time" for the last packet we sent on this "flow."
    // If you have multiple flows, you can store flow_id -> double.
    double prev_c_;

    // We store alpha from config (or read it directly from config_.alpha).
    double alpha_;

    // Helper: compute cost–reward and pick link (Algorithm 1).
    int selectLinkDChannel();
};

} // namespace ric

#endif // DCHANNEL_SCHEDULER_HPP
