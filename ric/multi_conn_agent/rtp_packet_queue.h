#pragma once

#include <unordered_map>
#include <deque>  // Changed from queue
#include <set>
#include <memory>
#include <mutex>
#include <vector>
#include <sys/time.h>
#include <thread>
#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include "../../logger/Logger.h"
#include "types.h"
#include "link_performance.h"

typedef uint16_t ue_id_t;
typedef uint32_t frame_id_t;
typedef uint16_t rlc_sn_t;


namespace multiconn {

class RTPPacketQueue {
private:
    struct UEQueue {
        std::deque<PDCPPacketInfo> pdcp_packets;
        timeval last_activity;
    };

    std::unordered_map<ue_id_t, std::unique_ptr<UEQueue>> ue_queues;
    std::mutex queues_mutex;
    static const size_t MAX_QUEUE_SIZE = 1000;

    std::atomic<bool> is_running;
    std::thread queue_thread;
    boost::lockfree::spsc_queue<PDCPPacketInfo, boost::lockfree::capacity<1024>> incoming_queue;

    std::shared_ptr<LinkPerformance> link_perf_;

    void process_queue();

public:
    RTPPacketQueue(std::shared_ptr<LinkPerformance> link_perf) : is_running(true), link_perf_(link_perf) {
        queue_thread = std::thread(&RTPPacketQueue::process_queue, this);
    }

    ~RTPPacketQueue() {
        is_running = false;
        if (queue_thread.joinable()) {
            queue_thread.join();
        }
    }

    using FrameCompletionCallback = std::function<void(ue_id_t, frame_id_t)>;
    FrameCompletionCallback frame_completion_callback_;
    void set_frame_completion_callback(FrameCompletionCallback callback) {
        frame_completion_callback_ = callback;
    }

    void add_ue_queue(ue_id_t ue_id);
    void remove_ue_queue(ue_id_t ue_id);
    void queue_pdcp_packet(const PDCPPacketInfo& packet_info);
    void update_rlc_info(const RLCInfo& rlc_info);
    void update_rlc_ack(const RLCInfo& rlc_info);
    std::vector<PDCPPacketInfo> get_frame_pdcp_packets(ue_id_t ue_id, frame_id_t frame_id);
    void cleanup_old_queues(int timeout_seconds = 300);
};

} // namespace multiconn
