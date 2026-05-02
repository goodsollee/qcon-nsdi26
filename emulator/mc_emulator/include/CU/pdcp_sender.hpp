#ifndef PDCP_SENDER_HPP
#define PDCP_SENDER_HPP

#include "pdcp_common.hpp"
#include "async_logger.hpp"
#include "pdcp_config.h"
#include "RIC/ric_interface_wrapper.hpp"
#include "qos_profiler.hpp"
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

// Define MAX_PATHS if not already defined
#define MAX_PATHS 8

class PdcpSender : public PdcpContext {
public:
    PdcpSender(const PdcpConfig& cfg);
    virtual ~PdcpSender();
    
    // Process a packet (add sequence number)
    virtual size_t processPacket(unsigned char* packet, size_t len) override;
    
    // Get current sequence number (for statistics)
    uint64_t getSequence() const;

    // Define ForwardingCallback type first
    using ForwardingCallback = std::function<bool(unsigned char*, size_t, uint8_t)>;
    
    // Then use it in the method declaration
    void setDeliveryCallback(ForwardingCallback callback) {
        forwardingCallback = std::move(callback);
    }

    // New methods for packet duplication
    void setDuplication(bool enabled);
    bool isDuplicationEnabled() const;
    void setDuplicationPaths(const std::vector<uint8_t>& paths);
    std::vector<uint8_t> getDuplicationPaths() const;

    // QoS profiler methods
    void setQosMode(QosProfiler::ClassificationMode mode);
    QosProfiler::ClassificationMode getQosMode() const;
    uint64_t getQueueStats(uint8_t queueId) const;
    void resetQosStats();

private:
    // Forwarding callback
    ForwardingCallback forwardingCallback;

    std::atomic<uint64_t> currentEpoch;
    std::atomic<uint64_t> nextSequence;

    AsyncLogger async_logger;
    
    // RIC interface
    std::unique_ptr<ric::RicInterfaceWrapper> ric_interface_;
    std::thread ric_hello_thread_;
    std::atomic<bool> ric_thread_running_;
    
    // Simple hello world thread function
    void ricHelloThread();

    std::vector<std::atomic<uint64_t>> path_bytes_transmitted;
    std::atomic<uint8_t> active_path{0};
    std::atomic<uint32_t> kpm_report_interval_ms{1000};
    std::thread kpm_metrics_thread_;
    std::atomic<bool> kpm_thread_running_{false};
    std::chrono::steady_clock::time_point last_kpm_report_time_;

    uint8_t total_link_num;
    
    // For KPM reporting
    void kpmMetricsThread();
    
    // For RC command handling
    void registerRicCommandHandlers();
    std::string handleSetPath(const std::string& params);
    std::string handleSetDuplication(const std::string& params); // New handler

    std::atomic<bool> duplication_enabled_;
    std::vector<uint8_t> duplication_paths_; // Paths to duplicate packets on
    mutable std::mutex duplication_paths_mutex_;

    // QoS profiler for queue selection
    std::unique_ptr<QosProfiler> qos_profiler_;

    // Multilink path support — set by handleSetPath when "is_multilink" arrives.
    // All members below have in-class initializers; constructor does NOT need
    // to mention them in its initializer list. Stored at end of class to avoid
    // disturbing existing member layout that some startup code may depend on.
    std::atomic<bool> multilink_active_{false};
    std::vector<uint8_t> multilink_paths_{};
    std::vector<double>  multilink_cum_ratios_{};
    std::mutex multilink_state_mutex_{};

    // Re-inject burst (paper §5.4): when RIC sends "reinject_burst" command,
    // sender duplicates the next N packets to alt path with HIGH priority queue
    // (queue_id=3 = highest in our 4-queue config). Approximates handover-style
    // packet move under our setup, which lacks an LCID-aware RLC reorder buffer
    // — duplicating ensures alt path delivers even when source link queue stalls.
    std::atomic<uint32_t> reinject_burst_remaining_{0};
    std::atomic<uint8_t>  reinject_alt_path_{0};
    std::string handleReinjectBurst(const std::string& params);
};

#endif /* PDCP_SENDER_HPP */