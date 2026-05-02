#include "scheduler/DChannel/dchannel_scheduler.hpp"
#include "log.hpp"
#include <cmath>
#include <limits>
#include <chrono>


const std::string MODULE = "DCHANNEL_SCHEDULER";

// We assume a fixed 1400B packet size for all transmissions:
static const double DCHANNEL_PACKET_SIZE_BYTES = 1400.0;

namespace ric {

DChannelScheduler::DChannelScheduler()
    : prev_c_(0.0),  // Now called prev_c_ (previous cost)
      alpha_(0.75)   // default, can be overridden in initialize()
{
    LOG_MODULE_INFO(MODULE, "DChannelScheduler created");
}

DChannelScheduler::~DChannelScheduler() {
    LOG_MODULE_INFO(MODULE, "DChannelScheduler destroyed");
}

bool DChannelScheduler::initialize(std::shared_ptr<StateManager> state_manager,
                                   const SchedulerConfig& config)
{
    // BaseScheduler's initialize
    if (!BaseScheduler::initialize(state_manager, config)) {
        return false;
    }
    LOG_MODULE_INFO(MODULE, "Initializing DChannelScheduler");

    // Load alpha from config
    alpha_ = config.dchannel_alpha; 
    LOG_MODULE_INFO(MODULE, "DChannelScheduler alpha=" << alpha_);

    // If needed, set up default link_info_ here (or rely on updateLinkMetrics).
    return true;
}

void DChannelScheduler::onLinkStateUpdated(int link_id, const LinkState& link_state)
{
    // We omit any direct use here; see updateLinkMetrics for link_info_ updates.
}

void DChannelScheduler::updateLinkMetrics(const std::map<int, LinkMetrics>& metrics)
{
    // Called by BaseScheduler with summarized link metrics
    // We'll fill in link_info_ from these:
    for (const auto& kv : metrics) {
        int link_id = kv.first - 1;
        const LinkMetrics& m = kv.second;

        DChannelLinkInfo info;
        
        // Here we just fix the link's "min_rtt_ms" to 5 ms 
        // for demonstration, but you can read from m.delay_ms 
        // or adapt to your logic:
        info.min_rtt_ms      = 5;              
        info.throughput_mbps = m.bandwidth_mbps; // from LinkMetrics
        info.buffer_bytes    = m.queue_size_bytes;   
        link_info_[link_id]  = info;

        LOG_MODULE_DEBUG(MODULE, "DChannelScheduler updated link=" << link_id
            << " rtt=" << info.min_rtt_ms << "ms"
            << " thr=" << info.throughput_mbps << "Mbps"
            << " buf=" << info.buffer_bytes << "B");
    }

    // Optionally run the scheduler once after an update:
    BaseScheduler::runOnce();
}

/**
 * decideLinkForPacket():
 * 
 * Implements the DChannel cost–reward approach (Algorithm 1).
 * 
 *   c_llc  = t_now + min_rtt_llc + [ ( packet_size + queue_llc ) / llc_bandwidth ]
 *   c_hbc  = t_now + min_rtt_hbc  (+ optionally a queue term for HBC)
 *   rewards= c_hbc - max( prev_c_, c_llc )
 *   cost   = ( packet_size + queue_llc ) / llc_bandwidth
 *   if rewards > alpha * cost => use LLC; otherwise use HBC.
 * 
 * - We assume exactly two links:
 *     link 0 => HBC 
 *     link 1 => LLC
 * - We also assume a single flow’s previous cost is stored in prev_c_.
 *   (If you have multiple flows, you can store a map<flow_id, double>.)
 */
int DChannelScheduler::decideLinkForPacket()
{
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Starting link decision algorithm");
    
    // If we don't have at least two links' info, just fallback to link 0 (HBC).
    if (link_info_.size() < 2) {
        LOG_MODULE_WARN(MODULE, "decideLinkForPacket(): Not enough links available, defaulting to HBC (link 0)");
        return 0;
    }

    // Retrieve link 0 => HBC, link 1 => LLC
    auto itHBC = link_info_.find(0);
    auto itLLC = link_info_.find(1);
    if (itHBC == link_info_.end() || itLLC == link_info_.end()) {
        LOG_MODULE_WARN(MODULE, "decideLinkForPacket(): Missing required link info for HBC (0) or LLC (1), defaulting to HBC");
        return 0; // fallback
    }

    // We'll assume current time "t_now=0" for a single-step approach.
    double t_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // Gather link metrics
    double rtt_hbc = itHBC->second.min_rtt_ms;
    double thr_hbc = itHBC->second.throughput_mbps;
    double buf_hbc = itHBC->second.buffer_bytes; // possibly unused

    double rtt_llc = itLLC->second.min_rtt_ms;
    double thr_llc = itLLC->second.throughput_mbps;
    double buf_llc = itLLC->second.buffer_bytes;

    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): HBC metrics - RTT: " << rtt_hbc << "ms, Throughput: " 
                    << thr_hbc << "Mbps, Buffer: " << buf_hbc << "B");
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): LLC metrics - RTT: " << rtt_llc << "ms, Throughput: " 
                    << thr_llc << "Mbps, Buffer: " << buf_llc << "B");

    // Convert throughput (Mbps) => bytes/ms:
    //   thr_llc (Mbps) * 1e6 => bits/s, /8 => bytes/s, /1000 => bytes/ms
    double hbc_bytes_per_ms = 0;
    if (thr_hbc != 0) {
        hbc_bytes_per_ms = (thr_hbc * 1e6) / 8.0 / 1000.0;
    }

    double llc_bytes_per_ms = 0;
    if (thr_llc != 0) {
        llc_bytes_per_ms = (thr_llc * 1e6) / 8.0 / 1000.0;
    }
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): LLC throughput converted: " << llc_bytes_per_ms << " bytes/ms");

    // c_hbc: ignoring queue for HBC
    double c_hbc = t_now 
                 + rtt_hbc
                 + ( (DCHANNEL_PACKET_SIZE_BYTES + buf_hbc ) / hbc_bytes_per_ms);

    // c_llc: includes queue + link throughput
    double c_llc = t_now 
                 + rtt_llc
                 + ( (DCHANNEL_PACKET_SIZE_BYTES + buf_llc ) / llc_bytes_per_ms );
    
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Cost HBC: " << c_hbc << "ms");
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Cost LLC: " << c_llc << "ms");
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Previous cost: " << prev_c_ << "ms");

    // reward = c_hbc - max(prev_c_, c_llc)
    double max_prev_c_llc = std::max(prev_c_, c_llc);
    double rewards = c_hbc - max_prev_c_llc;
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): max(prev_c_, c_llc): " << max_prev_c_llc << "ms");
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Reward: " << rewards << "ms");

    // cost = how many ms we block the LLC? 
    double cost = ( (DCHANNEL_PACKET_SIZE_BYTES + buf_llc ) / llc_bytes_per_ms );
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Cost: " << cost << "ms");
    LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): alpha * cost: " << (alpha_ * cost) << "ms (alpha=" << alpha_ << ")");

    // Compare with alpha * cost
    int chosenLink = 0; 
    double newCost = c_hbc;
    if (rewards > alpha_ * cost) {
        chosenLink = 1; 
        newCost    = c_llc;
        LOG_MODULE_INFO(MODULE, "decideLinkForPacket(): Chose LLC (link 1): reward (" << rewards 
                      << "ms) > alpha * cost (" << (alpha_ * cost) << "ms)");
    } else {
        LOG_MODULE_INFO(MODULE, "decideLinkForPacket(): Chose HBC (link 0): reward (" << rewards 
                      << "ms) <= alpha * cost (" << (alpha_ * cost) << "ms)");
    }

    // Update prev_c_ 
    // We interpret prev_c_ as "the relevant cost from the previous packet," i.e. 
    // the time we effectively finish sending that packet.
    double old_prev_c = prev_c_;
    if (newCost > prev_c_) {
        prev_c_ = newCost;
        LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Updated prev_c_: " << old_prev_c << "ms -> " << prev_c_ << "ms");
    } else {
        LOG_MODULE_DEBUG(MODULE, "decideLinkForPacket(): Maintained prev_c_: " << prev_c_ << "ms");
    }

    return chosenLink;
}

SchedulingDecision DChannelScheduler::makeDecisionInternal()
{
    // This function is used by BaseScheduler to pick a single link 
    // for an entire scheduling interval. In a pure per-packet approach, 
    // we'd do decideLinkForPacket() on every packet.
    // 
    // For demonstration, we just call decideLinkForPacket() once 
    // and set that link as the "selected_link_id."
    SchedulingDecision decision;
    decision.selected_link_id = decideLinkForPacket();
    decision.decision_reason  = "DChannel cost–reward step (using prev_c_)";
    decision.requires_path_change = true;
    return decision;
}

} // namespace ric
