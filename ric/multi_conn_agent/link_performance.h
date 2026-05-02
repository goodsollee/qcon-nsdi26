#pragma once
#include "types.h"
#include <map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <unordered_map>


namespace multiconn {

    struct LinkOptimizationResult {
        std::vector<link_id_t> selected_links;
        std::vector<double> split_ratios;
        double total_delivery_time_ms;
    };

    class LinkPerformance {
    public:
        struct LinkMetrics {
            double throughput_bps;
            double last_throughput_bps;
            uint32_t delivery_time_ms;
            bool has_valid_rlc;
            bool has_valid_delay;
            timestamp_t last_update;
            double link_quality_score;
        };

        struct LinkData {
            link_id_t link_id;
            double throughput_bps;
            double delay_ms;
            uint32_t queue_size;
            uint32_t priority;
        };

        explicit LinkPerformance(const std::vector<LinkConfig>& link_configs);
        ~LinkPerformance() {
            stop_periodic_calculation();
        }

        void start_periodic_calculation();
        void stop_periodic_calculation();

        void update_rlc_info(const RLCInfo& rlc_info, uint32_t tx_time);
        void update_delay_info(const DelayInfo& delay_info);

        void update_link_config(const LinkConfig& link_config);

        LinkOptimizationResult calculate_delivery_time(ue_id_t ue_id, uint32_t frame_size);
        
        LinkMetrics get_link_metrics(ue_id_t ue_id, link_id_t link_id) const;
        std::vector<std::pair<link_id_t, LinkMetrics>> get_all_link_metrics(ue_id_t ue_id) const;

    private:
        struct LinkMetricsState {
            struct {
                size_t queue_size{0};
                uint32_t acked_bytes{0};
                uint32_t last_acked_bytes{0};
                uint32_t tx_time{0};
                uint32_t last_tx_time{0};
                timestamp_t last_update{0};
            } rlc;

            struct {
                timestamp_t drx_wake_time{0};
                double xn_delay_ms{0};
                timestamp_t last_update{0};
            } delay;

            LinkMetrics current_metrics;
            LinkConfig config;
        };

        struct UEState {
            std::unordered_map<link_id_t, LinkMetricsState> link_metrics;
        };

        void periodic_calculation_loop();

        void calculate_throughput(ue_id_t ue_id, link_id_t link_id);
        void cleanup_old_metrics();
        void set_ewma_alpha(double alpha);

        std::atomic<bool> running_;
        std::thread calculation_thread_;
        std::mutex calculation_mutex_;
        std::condition_variable calculation_cv_;
        const uint32_t calculation_interval_ms_;

        mutable std::mutex metrics_mutex_;
        std::map<ue_id_t, UEState> ue_states_;
        std::unordered_map<link_id_t, LinkConfig> link_configs_;

        double alpha_;
        const double deadline_s_;
    };
}