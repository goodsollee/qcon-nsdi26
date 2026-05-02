#include "link_performance.h"
#include <algorithm>
#include <sys/time.h>

namespace multiconn {

LinkPerformance::LinkPerformance(const std::vector<LinkConfig>& link_configs)
    : running_(false), calculation_interval_ms_(20), alpha_(0.3), deadline_s_(0.1) {  // alpha = 0.3 for EWMA 
    for (const auto& config : link_configs) {
        link_configs_[config.link_id] = config;
        LOG_INFO(MODULE_NAME, "Initialized link: ", config.link_id, 
                 ", Name: ", config.link_name,
                 ", Priority: ", config.priority);
    } 
    start_periodic_calculation();
}

void LinkPerformance::start_periodic_calculation() {
    running_ = true;
    calculation_thread_ = std::thread(&LinkPerformance::periodic_calculation_loop, this);
    LOG_INFO(MODULE_NAME, "Started periodic throughput calculation thread");
}

void LinkPerformance::stop_periodic_calculation() {
    running_ = false;
    calculation_cv_.notify_one();
    if (calculation_thread_.joinable()) {
        calculation_thread_.join();
    }
    LOG_INFO(MODULE_NAME, "Stopped periodic throughput calculation thread");
}

void LinkPerformance::periodic_calculation_loop() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(calculation_mutex_);
            calculation_cv_.wait_for(lock, 
                std::chrono::milliseconds(calculation_interval_ms_),
                [this]() { return !running_; });
        }

        if (!running_) break;

        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        
        // Calculate throughput for all UEs and links
        for (auto& ue_pair : ue_states_) {
            for (auto& link_pair : ue_pair.second.link_metrics) {
                calculate_throughput(ue_pair.first, link_pair.first);
            }
        }
    }
}

void LinkPerformance::update_rlc_info(const RLCInfo& rlc_info, uint32_t tx_time) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto& ue_state = ue_states_[rlc_info.ue_id];
    auto& link_metric = ue_state.link_metrics[rlc_info.link_id];
    auto& rlc = link_metric.rlc;

    rlc.queue_size = rlc_info.queue_size;
    if (rlc_info.is_ack) {
        rlc.acked_bytes += rlc_info.acked_bytes;
        rlc.tx_time += tx_time;
    }
    rlc.last_update = rlc_info.timestamp;
     
    LOG_DEBUG(MODULE_NAME, "Updated RLC metrics - UE: ", rlc_info.ue_id,
             ", Link: ", rlc_info.link_id,
             ", Queue size: ", rlc_info.queue_size,
             ", Is ACK: ", rlc_info.is_ack,
             ", Acked bytes: ", rlc.acked_bytes,
             ", Tx time: ", rlc.tx_time);

    // calculate_throughput(rlc_info.ue_id, rlc_info.link_id);
    // calculate_link_quality(rlc_info.ue_id, rlc_info.link_id);
}

void LinkPerformance::update_delay_info(const DelayInfo& delay_info) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto& ue_state = ue_states_[delay_info.ue_id];
    auto& link_metrics = ue_state.link_metrics[delay_info.link_id];
    auto& delay = link_metrics.delay;

    delay.drx_wake_time = delay_info.drx_wake_time;
    delay.xn_delay_ms = delay_info.xn_delay_ms;
    delay.last_update = delay_info.timestamp;

    LOG_DEBUG(MODULE_NAME, "Updated delay metrics - UE: ", delay_info.ue_id,
             ", Link: ", delay_info.link_id,
             ", DRX wake time: ", delay_info.drx_wake_time,
             ", Xn delay: ", delay_info.xn_delay_ms, "ms");

    calculate_delivery_time(delay_info.ue_id, delay_info.link_id);
    //calculate_link_quality(delay_info.ue_id, delay_info.link_id);
}

LinkPerformance::LinkMetrics LinkPerformance::get_link_metrics(ue_id_t ue_id, link_id_t link_id) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto ue_it = ue_states_.find(ue_id);
    if (ue_it == ue_states_.end()) {
        LOG_WARNING(MODULE_NAME, "No metrics found for UE: ", ue_id);
        return LinkMetrics{};
    }

    auto link_it = ue_it->second.link_metrics.find(link_id);
    if (link_it == ue_it->second.link_metrics.end()) {
        LOG_WARNING(MODULE_NAME, "No metrics found for Link: ", link_id, " UE: ", ue_id);
        return LinkMetrics{};
    }

    return link_it->second.current_metrics;
}

void LinkPerformance::calculate_throughput(ue_id_t ue_id, link_id_t link_id) {
    auto& link_metrics = ue_states_[ue_id].link_metrics[link_id];
    auto& rlc = link_metrics.rlc;
    auto& metrics = link_metrics.current_metrics;

    // Calculate throughput based on bytes acknowledged since last calculation
    uint32_t bytes_diff = rlc.acked_bytes - rlc.last_acked_bytes;
    uint32_t time_diff_us = rlc.tx_time - rlc.last_tx_time;

    double instant_throughput = 0.0;
    if (time_diff_us > 0) {
        // Convert to bps: (bytes * 8 bits/byte) / (time_us) / 1e6 bits/bps
        instant_throughput = (bytes_diff * 8.0) / (time_diff_us /1e6);
    }

    // Apply EWMA formula: new_avg = α * instant_value + (1 - α) * last_avg
    if (metrics.last_throughput_bps == 0.0) {
        // First measurement, use instantaneous value
        metrics.throughput_bps = instant_throughput;
    } else {
        // Calculate EWMA
        metrics.throughput_bps = alpha_ * instant_throughput + 
                                (1.0 - alpha_) * metrics.last_throughput_bps;
    }

    // Store current throughput for next calculation
    metrics.last_throughput_bps = metrics.throughput_bps;

    // Update last values for next calculation
    rlc.last_acked_bytes = rlc.acked_bytes;
    rlc.last_tx_time = rlc.tx_time;

    LOG_DEBUG(MODULE_NAME, "Calculated throughput - UE: ", ue_id,
                ", Link: ", link_id,
                ", Instant Throughput: ", instant_throughput, " bps",
                ", EWMA Throughput: ", metrics.throughput_bps, " bps",
                ", Bytes diff: ", bytes_diff,
                ", Time diff: ", time_diff_us, "us");
}

LinkOptimizationResult LinkPerformance::calculate_delivery_time(ue_id_t ue_id, uint32_t frame_size) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
        
    auto ue_it = ue_states_.find(ue_id);
    if (ue_it == ue_states_.end()) {
        LOG_WARNING(MODULE_NAME, "No metrics found for UE: ", ue_id);
        return LinkOptimizationResult{};
    }

    // Collect valid links and their metrics
    std::vector<LinkData> valid_links;
    for (const auto& [link_id, metrics] : ue_it->second.link_metrics) {
        // Skip links with no valid metrics or zero throughput
        if (metrics.current_metrics.throughput_bps <= 0) {
            continue;
        }
        
        LinkData link_data{
            .link_id = link_id,
            .throughput_bps = metrics.current_metrics.throughput_bps,
            .delay_ms = metrics.delay.xn_delay_ms,
            .queue_size = metrics.rlc.queue_size,
            .priority = metrics.config.priority
        };

        // Add DRX delay if applicable
        if (metrics.delay.drx_wake_time > 0) {
            struct timeval now;
            gettimeofday(&now, nullptr);
            uint64_t current_time = now.tv_sec * 1000000 + now.tv_usec;
            
            if (metrics.delay.drx_wake_time > current_time) {
                link_data.delay_ms += (metrics.delay.drx_wake_time - current_time) / 1000;
            }
        }
        
        link_data.delay_ms /= 1000; // Convert to seconds
        link_data.delay_ms += (metrics.rlc.queue_size * 8) / link_data.throughput_bps; // Add queueing delay

        valid_links.push_back(link_data);
    }

    if (valid_links.empty()) {
        LOG_WARNING(MODULE_NAME, "No valid links found for UE ", ue_id, ". Details - ");
        // Print all link metrics to understand why they were invalid
        for (const auto& [link_id, metrics] : ue_it->second.link_metrics) {
            LOG_WARNING(MODULE_NAME, "Link ", link_id, 
            ": throughput=", metrics.current_metrics.throughput_bps, " bps",
            ", xn_delay=", metrics.delay.xn_delay_ms, " ms",
            ", queue_size=", metrics.rlc.queue_size,
            ", drx_wake_time=", metrics.delay.drx_wake_time,
            (metrics.current_metrics.throughput_bps <= 0 ? " (Rejected: zero/negative throughput)" : ""));
        }
        return LinkOptimizationResult{};
    }


    // Sort links by frame completion time
    std::sort(valid_links.begin(), valid_links.end(),
        [frame_size](const LinkData& a, const LinkData& b) {
            return (a.priority) < (b.priority);
        });

    LinkOptimizationResult best_result;
    best_result.total_delivery_time_ms = UINT32_MAX;

    // Only consider top 2 links to reduce complexity
    /*
    if (valid_links.size() > 2) {
        valid_links.resize(2);
    }
    */

    // Try with increasing number of links
    for (size_t k = 1; k <= valid_links.size(); k++) {
        // Calculate sum of throughputs and weighted delays
        double sum_tp = 0;
        double sum_weighted_delays = 0;
        
        for (size_t i = 0; i < k; i++) {
            sum_tp += valid_links[i].throughput_bps;
            sum_weighted_delays += valid_links[i].delay_ms * valid_links[i].throughput_bps;
        }

        // Calculate completion time T
        // T = (F + Σ(di * tpi)) / Σ(tpi)
        double frame_bits = frame_size * 8.0; // Convert bytes to bits
        double T = static_cast<double>((frame_bits + sum_weighted_delays) / sum_tp); // Unit: second

        LOG_DEBUG(MODULE_NAME, 
            "Trying solution with k=", k,
            " | sum_tp=", sum_tp,
            " | sum_weighted_delays=", sum_weighted_delays,
            " | T=", T, " s");

        // Check if this solution is feasible
        bool feasible = true;
        std::vector<double> splits;
        splits.reserve(k);

        double total_split = 0;
        for (size_t i = 0; i < k && feasible; i++) {
            // Calculate split ratio for each link: fi = (T - di) * tpi
            double split = std::max(0.0, static_cast<double>(T - valid_links[i].delay_ms)) *
                            valid_links[i].throughput_bps / frame_bits;

            LOG_DEBUG(MODULE_NAME,
                "Link ", valid_links[i].link_id,
                " | T=", T,
                " | delay=", valid_links[i].delay_ms,
                " | throughput=", valid_links[i].throughput_bps,
                " | calculated split=", split);
            
            // Check if split is valid
            if (split < 0 || T < valid_links[i].delay_ms) {
                LOG_DEBUG(MODULE_NAME, 
                    "Solution infeasible: invalid split for Link ", valid_links[i].link_id,
                    " | split=", split,
                    " | T=", T,
                    " | delay=", valid_links[i].delay_ms);
                feasible = false;
                break;
            }
            
            splits.push_back(split);
            total_split += split;
        }

        // Check if total split is approximately 1.0 (allowing for small numerical errors)
        if (std::abs(total_split - 1.0) > 0.001) {
            LOG_DEBUG(MODULE_NAME, 
                "Solution infeasible: total split not equal to 1.0",
                " | total_split=", total_split,
                " | difference=", std::abs(total_split - 1.0));
            feasible = false;
        }

        // Check deadline constraint
        if (feasible && T <= deadline_s_ && T < best_result.total_delivery_time_ms) {
            LOG_DEBUG(MODULE_NAME,
            "Found feasible solution",
            " | k=", k,
            " | T=", T,
            " | deadline=", deadline_s_,
            " | current_best=", best_result.total_delivery_time_ms);

            best_result.selected_links.clear();
            best_result.split_ratios.clear();
            best_result.total_delivery_time_ms = T;

            for (size_t i = 0; i < k; i++) {
                best_result.selected_links.push_back(valid_links[i].link_id);
                best_result.split_ratios.push_back(splits[i]);
            }

            // If we found a solution with minimal links that meets deadline, we can stop
            if (T <= deadline_s_) {
                break;
            }
        }
    }

    // Log the result
    if (best_result.selected_links.empty()) {
        LOG_WARNING(MODULE_NAME, 
        "No feasible solution found for UE ", ue_id,
        " | Frame size: ", frame_size, " bytes (", frame_size * 8, " bits)",
        " | Deadline constraint: ", deadline_s_, " s", 
        " | Considered links details");
    
        for (const auto& link : valid_links) {
            LOG_WARNING(MODULE_NAME, "Link ", link.link_id, 
            ": throughput=", link.throughput_bps, " bps",
            ", total_delay=", link.delay_ms, " s",
            ", queue_size=", link.queue_size,
            ", estimated completion time=", 
            (link.delay_ms + frame_size*8/link.throughput_bps), " s");
        } 
    } else {
        std::string split_info;
        for (size_t i = 0; i < best_result.selected_links.size(); i++) {
            split_info += "Link " + std::to_string(best_result.selected_links[i]) + 
                        ": " + std::to_string(best_result.split_ratios[i] * 100) + "% ";
        }
        LOG_DEBUG(MODULE_NAME, "Selected links for UE ", ue_id, ": ", split_info,
                    "Total delivery time: ", best_result.total_delivery_time_ms, " s");
    }

    return best_result;
}

void LinkPerformance::set_ewma_alpha(double alpha) {
    if (alpha >= 0.0 && alpha <= 1.0) {
        alpha_ = alpha;
        LOG_INFO(MODULE_NAME, "Updated EWMA alpha to: ", alpha);
    } else {
        LOG_WARNING(MODULE_NAME, "Invalid EWMA alpha value: ", alpha, 
                    ". Must be between 0 and 1. Keeping current value: ", alpha_);
    }
}

void LinkPerformance::update_link_config(const LinkConfig& link_config) {
    
    link_configs_[link_config.link_id] = link_config;
    LOG_INFO(MODULE_NAME, "Updated link configuration - Link: ", link_config.link_id,
             ", Name: ", link_config.link_name,
             ", Priority: ", link_config.priority);
}

} // namespace multiconn