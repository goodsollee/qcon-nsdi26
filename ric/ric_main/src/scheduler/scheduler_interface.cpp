#include "scheduler/scheduler_interface.hpp"
#include "RIC/controller/cu_scheduler_controller.hpp"
#include "scheduler/Single/single_scheduler.hpp" // Add this include
#include "scheduler/minRTT/minrtt_scheduler.hpp" // Add this include
#include "scheduler/DChannel/dchannel_scheduler.hpp"
#include "scheduler/PD/duplication_scheduler.hpp"
#include "scheduler/QCON/qcon_scheduler.hpp"
#include "scheduler/FiveQI/fiveqi_scheduler.hpp"
#include "log.hpp"
#include <algorithm>
#include <regex>
#include <sstream>
#include <thread>
#include <atomic>

const std::string MODULE = "BASE_SCHEDULER";

namespace ric {

BaseScheduler::BaseScheduler()
    : last_switch_time_(std::chrono::steady_clock::now()),
      running_(false) {
    LOG_MODULE_DEBUG(MODULE, "BaseScheduler created");
}

BaseScheduler::~BaseScheduler() {
    stop();
    LOG_MODULE_DEBUG(MODULE, "BaseScheduler destroyed");
}

bool BaseScheduler::initialize(std::shared_ptr<StateManager> state_manager, 
                             const SchedulerConfig& config) {
    LOG_MODULE_INFO(MODULE, "Initializing");

    state_manager_ = state_manager;
    config_ = config;
    
    // Register as listener for state updates
    state_manager_->registerListener(this);
    
    LOG_MODULE_INFO(MODULE, "Initialized " << config_.scheduler_name << " scheduler");
    
    return true;
}

void BaseScheduler::setController(std::shared_ptr<CuSchedulerController> controller) {
    controller_ = controller;
    LOG_MODULE_INFO(MODULE, "Set controller for " << config_.scheduler_name << " scheduler");
}

SchedulingDecision BaseScheduler::makeDecision() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update statistics
    stats_.total_decisions++;
    
    // Get decision from scheduler-specific logic
    SchedulingDecision decision = makeDecisionInternal();
    
    return decision;
}

bool BaseScheduler::executeDecision(const SchedulingDecision& decision) {
    if (!controller_) {
        LOG_MODULE_ERROR(MODULE, "Cannot execute decision: no controller set");
        return false;
    }

    bool success = true;
    // Honor decision.requires_path_change: when the scheduler tells us nothing has
    // changed, do NOT re-issue the same set_path RC every tick. The CU side handles
    // each set_path by toggling multilink_active_/active_path which can race with
    // in-flight packet routing and (worse) make WebRTC's CCA see jittery delivery
    // because the data-plane re-binds queues. Skipping no-op sends keeps the
    // pipeline stable between actual scheduler decisions.
    if (decision.requires_path_change) {
        if (decision.is_multipath && decision.split_ratio.size() > 1) {
            controller_->sendMultilinkCommandImmediate(decision.split_ratio);
        } else {
            controller_->sendCommandImmediate(decision.selected_link_id);
        }
    }

    if (success) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_link_id_ = decision.selected_link_id;
    }
    return success;
}

bool BaseScheduler::runOnce() {
    SchedulingDecision decision = makeDecision();
    return executeDecision(decision);
}

bool BaseScheduler::start(int interval_ms) {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    
    if (running_) {
        LOG_MODULE_WARN(MODULE, "Scheduler already running");
        return true;
    }
    
    if (!controller_) {
        LOG_MODULE_ERROR(MODULE, "Cannot start scheduler: no controller set");
        return false;
    }
    
    running_ = true;
    scheduler_thread_ = std::thread(&BaseScheduler::schedulerThreadFunc, this, interval_ms);
    
    LOG_MODULE_INFO(MODULE, "Started " << config_.scheduler_name << " scheduler with interval " << interval_ms << "ms");
    return true;
}

void BaseScheduler::stop() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Wait for thread to finish
    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }
    
    LOG_MODULE_INFO(MODULE, "Stopped " << config_.scheduler_name << " scheduler");
}

void BaseScheduler::setPathChangeCallback(std::function<void(const std::string&, int)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_change_callback_ = callback;
}

int BaseScheduler::getCurrentLinkId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_link_id_;
}

SchedulerConfig BaseScheduler::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void BaseScheduler::updateConfig(const SchedulerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    LOG_MODULE_INFO(MODULE, "Updated scheduler configuration");
}

Json::Value BaseScheduler::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Json::Value stats;
    stats["scheduler_name"] = config_.scheduler_name;
    stats["current_link"] = current_link_id_;
    stats["total_decisions"] = stats_.total_decisions;
    stats["path_changes"] = stats_.path_changes;
    stats["hysteresis_prevented_changes"] = stats_.hysteresis_prevented_changes;
    
    // Calculate uptime in seconds
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats_.start_time
    ).count();
    stats["uptime_seconds"] = static_cast<Json::UInt64>(uptime);
    
    // Calculate time since last switch
    auto time_since_switch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_switch_time_
    ).count();
    stats["time_since_last_switch_ms"] = static_cast<Json::UInt64>(time_since_switch);
    
    return stats;
}

void BaseScheduler::onLinkStateUpdated(int link_id, const LinkState& link_state) {
    // Base implementation does nothing
    // Derived classes can override this to react to link state updates
    LOG_MODULE_DEBUG(MODULE, "Link state updated for " << link_id);
}

void BaseScheduler::onSystemStateUpdated(const Json::Value& system_state) {
    LOG_MODULE_INFO(MODULE, "System state updated");
    
    std::map<int, LinkMetrics> link_metrics;
    
    try {
        // Extract link states if available
        if (system_state.isMember("links") && system_state["links"].isArray()) {
            const Json::Value& links = system_state["links"];
            
            // Iterate through all links in the array
            for (const auto& link : links) {
                if (!link.isMember("link_id")) {
                    LOG_MODULE_WARN(MODULE, "Link missing link_id field");
                    continue;
                }
                
                int link_id = link["link_id"].asInt();
                LinkMetrics metrics;
                
                // Extract metrics from LinkState format
                if (link.isMember("throughput_mbps")) {
                    metrics.bandwidth_mbps = link["throughput_mbps"].asDouble();
                }
                if (link.isMember("latest_e2e_delay_ms")) {
                    metrics.delay_ms = link["latest_e2e_delay_ms"].asDouble();
                }
                if (link.isMember("packet_loss_rate")) {
                    metrics.loss_percent = link["packet_loss_rate"].asDouble() * 100.0; // Convert from 0-1 to percentage
                }
                if (link.isMember("buffer_occupancy_bytes")) {
                    metrics.queue_size_bytes = link["buffer_occupancy_bytes"].asUInt64();
                    if (metrics.queue_size_bytes == 0) {
                        metrics.delay_ms = 0;
                    }
                }
                if (link.isMember("rlc_acked_bytes")) {
                    metrics.acked_bytes = link["rlc_acked_bytes"].asUInt64();
                }
                if (link.isMember("pdcp_transmitted_bytes")) {
                    metrics.pdcp_transmitted_bytes = link["pdcp_transmitted_bytes"].asUInt64();
                }
                
                // Check if link is active
                bool active = link.get("active", false).asBool();
                if (!active) {
                    LOG_MODULE_DEBUG(MODULE, "Skipping inactive link " << link_id);
                    continue;
                }
                
                link_metrics[link_id] = metrics;
                LOG_MODULE_DEBUG(MODULE, "Link " << link_id << ": bandwidth=" << metrics.bandwidth_mbps 
                         << " Mbps, delay=" << metrics.delay_ms << " ms, loss=" << metrics.loss_percent << "%"
                         << ", queue_size=" << metrics.queue_size_bytes << " bytes, acked_bytes=" << metrics.acked_bytes);
            }
            
            // Update link metrics in the scheduler
            updateLinkMetrics(link_metrics);
        } else {
            LOG_MODULE_DEBUG(MODULE, "No links array found in system state update");
        }
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Error processing system state update: " << e.what());
    }
}

bool BaseScheduler::shouldSwitchLinks(
    const std::string& candidate_link_id, 
    double candidate_score, 
    double current_score
) {
    
    // Check time since last switch
    auto time_since_switch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_switch_time_
    ).count();
    
    if (time_since_switch < config_.min_switch_interval_ms) {
        LOG_MODULE_DEBUG(MODULE, "Too soon to switch: " << time_since_switch << "ms < " 
                 << config_.min_switch_interval_ms << "ms");
        return false;
    }
    
    // Check if improvement is significant enough
    double improvement = candidate_score - current_score;
    if (improvement < config_.link_switch_threshold) {
        LOG_MODULE_DEBUG(MODULE, "Improvement not significant: " << improvement << " < " 
                 << config_.link_switch_threshold);
        return false;
    }
    
    return true;
}

int BaseScheduler::extractPathFromLinkId(const std::string& link_id) {
    // Extract path index from link ID (e.g., "link_1" -> 1)
    std::regex re("link_([0-9]+)");
    std::smatch match;
    
    if (std::regex_search(link_id, match, re) && match.size() > 1) {
        try {
            return std::stoi(match[1]);
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(MODULE, "Failed to parse path number from link ID " << link_id << ": " << e.what());
        }
    }
    
    // Default to 0 if no path found
    return 0;
}

void BaseScheduler::schedulerThreadFunc(int interval_ms) {
    LOG_MODULE_ERROR(MODULE, "Scheduler thread started with interval " << interval_ms << "ms");

    int tick = 0;
    auto thread_start = std::chrono::steady_clock::now();
    while (running_) {
        try {
            // Heartbeat every 5 seconds to detect silent thread freezes
            ++tick;
            if (tick % (5000 / interval_ms == 0 ? 50 : 5000 / interval_ms) == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - thread_start).count();
                LOG_MODULE_ERROR(MODULE, "Scheduler heartbeat: tick=" << tick << ", elapsed=" << elapsed << "s, running_=" << running_.load());
            }

            // Make and execute a decision
            runOnce();

            // Sleep for the interval
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        } catch (const zmq::error_t& e) {
           if (e.num() == ETERM) {                     // context gone
               LOG_MODULE_INFO(MODULE,
                   "ZMQ context terminated – leaving scheduler thread");
               break;
           }
           LOG_MODULE_ERROR(MODULE, "ZMQ error in scheduler thread: " << e.what());
           std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
           // Catch ALL std exceptions so the scheduler thread doesn't silently
           // die on first hiccup (e.g., out_of_range from a bad map lookup).
           // Without this, microbench traces with sparse data caused thread
           // termination at ~13s, leaving scheduler dormant for the rest of
           // the trace.
           LOG_MODULE_ERROR(MODULE, "Unhandled std::exception in scheduler thread: "
               << e.what() << " — continuing");
           std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (...) {
           LOG_MODULE_ERROR(MODULE, "Unknown exception in scheduler thread — continuing");
           std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    LOG_MODULE_INFO(MODULE, "Scheduler thread stopped");
}

// Factory implementation
std::shared_ptr<SchedulerInterface> SchedulerFactory::createScheduler(
    const std::string& name,
    std::shared_ptr<StateManager> state_manager,
    const SchedulerConfig& config,
    std::shared_ptr<CuSchedulerController> controller
) {
    // Configure the factory to create different scheduler types
    
    SchedulerConfig actualConfig = config;
    actualConfig.scheduler_name = name;

    LOG_MODULE_INFO(MODULE, "Creating scheduler of type: " << name);
    
    std::shared_ptr<SchedulerInterface> scheduler = nullptr;
    
    if (name == "single") {
        auto single_scheduler = std::make_shared<SingleScheduler>();
        // Use dynamic_pointer_cast instead of static_pointer_cast
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(single_scheduler);
    } else if (name == "minrtt") {
        auto minrtt_scheduler = std::make_shared<MinRttScheduler>();
        // Use dynamic_pointer_cast instead of static_pointer_cast
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(minrtt_scheduler);
    } else if (name == "dchannel") {
        auto dchannel_scheduler = std::make_shared<DChannelScheduler>();
        // Use dynamic_pointer_cast instead of static_pointer_cast
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(dchannel_scheduler);
    } else if (name == "pd") {
        // Create packet duplication scheduler
        auto pd_scheduler = std::make_shared<PacketDuplicationScheduler>();
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(pd_scheduler);
    } else if (name == "qos" || name == "5qi") {
        auto fiveqi_scheduler = std::make_shared<FiveQiScheduler>();
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(fiveqi_scheduler);
    } else if (name == "qcon") {
        // Create and return QCON scheduler
        auto qcon_scheduler = std::make_shared<QconScheduler>();
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(qcon_scheduler);
    } else if (name == "ideal") {
        auto ideal_scheduler = std::make_shared<SingleScheduler>();
        scheduler = std::dynamic_pointer_cast<SchedulerInterface>(ideal_scheduler);
    } 
    else {
        LOG_MODULE_ERROR(MODULE, "Unknown scheduler type: " << name);
        return nullptr;
    }
    
    // If we created a scheduler, initialize it
    if (scheduler) {
        if (!scheduler->initialize(state_manager, actualConfig)) {
            LOG_MODULE_ERROR(MODULE, "Failed to initialize " << name << " scheduler");
            return nullptr;
        }
        
        scheduler->setController(controller);
        LOG_MODULE_INFO(MODULE, "Successfully created and initialized " << name << " scheduler");
    }
    
    return scheduler;
}

} // namespace ric