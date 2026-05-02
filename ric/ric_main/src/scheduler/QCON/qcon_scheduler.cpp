#include "scheduler/QCON/qcon_scheduler.hpp"
#include "RIC/controller/cu_scheduler_controller.hpp"
#include "log.hpp"
#include "async_logger.hpp"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <functional>
#include <cmath>

namespace ric {

const std::string MODULE = "QCON_SCHEDULER";

QconScheduler::QconScheduler() {
    LOG_MODULE_DEBUG(MODULE, "QconScheduler created");

    // Initialize statistics
    stats_.primary_link_selections = 0;
    stats_.backup_link_selections = 0;
    stats_.forwarding_events = 0;
    stats_.deadline_adjustments = 0;
    stats_.avg_frame_completion_time = 0.0;
    stats_.avg_frame_deadline = 0.0;
    stats_.avg_jitter = 0.0;
    
    // Per-frame deadline used by Algorithm 1; default 100ms to retain prior behaviour.
    // Override via QCON_APP_DEADLINE_MS env var for testing tighter (e.g., 33ms ~ 30fps)
    // budgets that force the multilink branch when single-link can't meet the deadline.
    app_deadline_ = 100;
    if (const char* env = std::getenv("QCON_APP_DEADLINE_MS")) {
        try { app_deadline_ = static_cast<uint32_t>(std::stoul(env)); } catch(...) {}
    }
    frame_arrival_order_initialized_ = false;

    // Setup CSV log directory: prefer QCON_LOG_DIR env var (set by run scripts).
    // If unset, default to ./qcon_logs/ relative to cwd; mkdir -p so it always exists.
    const char* env_log_dir = std::getenv("QCON_LOG_DIR");
    if (env_log_dir && env_log_dir[0] != '\0') {
        log_dir_ = env_log_dir;
    } else {
        log_dir_ = "./qcon_logs";
    }
    // Ensure trailing slash for downstream code that concatenates filename
    if (!log_dir_.empty() && log_dir_.back() != '/') log_dir_ += '/';
    std::string mkdir_cmd = "mkdir -p '" + log_dir_ + "' 2>/dev/null";
    (void)system(mkdir_cmd.c_str());
    LOG_MODULE_INFO(MODULE, "Using log directory: " << log_dir_);
    setupLogFiles();
}

QconScheduler::~QconScheduler() {
    // Close async loggers (will happen automatically when unique_ptr is destroyed)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        qoe_logger_.reset();
        scheduling_logger_.reset();
        deadline_logger_.reset();
        frame_lifecycle_logger_.reset();
    }
    
    LOG_MODULE_DEBUG(MODULE, "QconScheduler destroyed");
}

bool QconScheduler::initialize(std::shared_ptr<StateManager> state_manager, 
                             const SchedulerConfig& config) {
    LOG_MODULE_INFO(MODULE, "Initializing QCON scheduler");
    
    if (!BaseScheduler::initialize(state_manager, config)) {
        return false;
    }
    
    // Note: We don't register frame completion callback here
    // It will be registered in setQoEProcessor when we have a valid QoE processor
    
    // Constructor already set up log_dir_ + log files; if somehow empty, set up here.
    if (log_dir_.empty()) {
        const char* env_log_dir = std::getenv("QCON_LOG_DIR");
        log_dir_ = (env_log_dir && env_log_dir[0]) ? env_log_dir : "./qcon_logs";
        if (log_dir_.back() != '/') log_dir_ += '/';
        std::string mkdir_cmd = "mkdir -p '" + log_dir_ + "' 2>/dev/null";
        (void)system(mkdir_cmd.c_str());
        LOG_MODULE_INFO(MODULE, "Using log directory: " << log_dir_);
        setupLogFiles();
    }

    // Check for and load config.csv if available — env-driven path
    const char* env_cfg_csv = std::getenv("QCON_CONFIG_CSV");
    std::string config_csv_path = (env_cfg_csv && env_cfg_csv[0]) ? env_cfg_csv : "";
    if (!config_csv_path.empty() && access(config_csv_path.c_str(), F_OK) == 0) {
        LOG_MODULE_INFO(MODULE, "Found config.csv at " << config_csv_path << ", loading configuration...");
        if (!loadConfigFromCsv(config_csv_path)) {
            LOG_MODULE_WARN(MODULE, "Failed to load configuration from config.csv, using default config");
        }
    } else {
        LOG_MODULE_INFO(MODULE, "No QCON_CONFIG_CSV set or file missing, using default configuration");
    }
    
    LOG_MODULE_INFO(MODULE, "QCON scheduler initialized");
    return true;
}

void QconScheduler::setQoEProcessor(std::shared_ptr<QoEProcessor> qoe_processor) {
    qoe_processor_ = qoe_processor;
    
    if (qoe_processor_) {
        // Register for frame completion notifications
        /*
        qoe_processor_->setFrameCompletionCallback(
            std::bind(&QconScheduler::handleFrameCompletion, this, 
                    std::placeholders::_1, std::placeholders::_2));
        */
        // Register for new frame notifications - trigger runOnce()
        qoe_processor_->setNewFrameCallback(
            std::bind(&QconScheduler::handleNewFrame, this));
        
        LOG_MODULE_INFO(MODULE, "QoE processor set for QCON scheduler with frame callbacks");
    }
}

void QconScheduler::enablePacketForwarding(bool enable) {
    qcon_config_.enable_packet_forwarding = enable;
    LOG_MODULE_INFO(MODULE, "Packet forwarding " << (enable ? "enabled" : "disabled"));
}

void QconScheduler::updateQconConfig(const QconSchedulerConfig& config) {
    qcon_config_ = config;
    LOG_MODULE_INFO(MODULE, "QCON configuration updated");
}

QconSchedulerConfig QconScheduler::getQconConfig() const {
    return qcon_config_;
}

bool QconScheduler::loadConfigFromCsv(const std::string& csv_path) {
    LOG_MODULE_INFO(MODULE, "Loading QCON scheduler configuration from: " << csv_path);
    
    // Open CSV file
    std::ifstream csv_file(csv_path);
    if (!csv_file.is_open()) {
        LOG_MODULE_ERROR(MODULE, "Failed to open config file: " << csv_path);
        return false;
    }
    
    // Create a copy of the current configuration to modify
    QconSchedulerConfig new_config = qcon_config_;
    
    // Parse CSV file
    std::string line;
    try {
        // Skip header line
        if (!std::getline(csv_file, line)) {
            LOG_MODULE_ERROR(MODULE, "CSV file is empty");
            return false;
        }
        
        // Process each configuration line
        while (std::getline(csv_file, line)) {
            // Skip empty lines
            if (line.empty()) {
                continue;
            }
            
            // Split by comma
            std::istringstream ss(line);
            std::string param_name, param_value;
            
            if (std::getline(ss, param_name, ',') && std::getline(ss, param_value, ',')) {
                // Trim whitespace
                param_name.erase(0, param_name.find_first_not_of(" \t"));
                param_name.erase(param_name.find_last_not_of(" \t") + 1);
                param_value.erase(0, param_value.find_first_not_of(" \t"));
                param_value.erase(param_value.find_last_not_of(" \t") + 1);
                
                // Convert value to appropriate type and update configuration
                try {
                    if (param_name == "jitter_threshold") {
                        new_config.jitter_threshold = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set jitter_threshold = " << new_config.jitter_threshold);
                    } 
                    else if (param_name == "deadline_adjustment_factor") {
                        new_config.deadline_adjustment_factor = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set deadline_adjustment_factor = " << new_config.deadline_adjustment_factor);
                    }
                    else if (param_name == "primary_link_preference") {
                        new_config.primary_link_preference = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set primary_link_preference = " << new_config.primary_link_preference);
                    }
                    else if (param_name == "backup_link_threshold") {
                        new_config.backup_link_threshold = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set backup_link_threshold = " << new_config.backup_link_threshold);
                    }
                    else if (param_name == "enable_packet_forwarding") {
                        new_config.enable_packet_forwarding = (param_value == "true" || param_value == "1");
                        LOG_MODULE_INFO(MODULE, "Set enable_packet_forwarding = " << new_config.enable_packet_forwarding);
                    }
                    else if (param_name == "delay_threshold_for_forwarding") {
                        new_config.delay_threshold_for_forwarding = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set delay_threshold_for_forwarding = " << new_config.delay_threshold_for_forwarding);
                    }
                    else if (param_name == "enable_multilink_scheduling") {
                        new_config.enable_multilink_scheduling = (param_value == "true" || param_value == "1");
                        LOG_MODULE_INFO(MODULE, "Set enable_multilink_scheduling = " << new_config.enable_multilink_scheduling);
                    }
                    else if (param_name == "avg_frame_size_kb") {
                        new_config.avg_frame_size_kb = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set avg_frame_size_kb = " << new_config.avg_frame_size_kb);
                    }
                    else if (param_name == "min_chunk_size_kb") {
                        new_config.min_chunk_size_kb = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set min_chunk_size_kb = " << new_config.min_chunk_size_kb);
                    }
                    else if (param_name == "deadline_threshold") {
                        new_config.deadline_threshold = std::stod(param_value);
                        LOG_MODULE_INFO(MODULE, "Set deadline_threshold = " << new_config.deadline_threshold);
                    }
                    else if (param_name == "use_cost_based_selection") {
                        new_config.use_cost_based_selection = (param_value == "true" || param_value == "1");
                        LOG_MODULE_INFO(MODULE, "Set use_cost_based_selection = " << new_config.use_cost_based_selection);
                    }
                    else {
                        LOG_MODULE_WARN(MODULE, "Unknown configuration parameter: " << param_name);
                    }
                }
                catch (const std::exception& e) {
                    LOG_MODULE_ERROR(MODULE, "Error parsing parameter " << param_name << ": " << e.what());
                }
            }
        }
        
        // Update the configuration
        updateQconConfig(new_config);
        LOG_MODULE_INFO(MODULE, "Successfully loaded QCON scheduler configuration from CSV");
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Error loading configuration from CSV: " << e.what());
        return false;
    }
}

Json::Value QconScheduler::getExtendedStatistics() const {
    Json::Value stats;
    
    // Add base scheduler statistics
    stats = BaseScheduler::getStatistics();
    
    // Add QCON-specific statistics
    stats["primary_link_selections"] = static_cast<Json::UInt64>(stats_.primary_link_selections);
    stats["backup_link_selections"] = static_cast<Json::UInt64>(stats_.backup_link_selections);
    stats["forwarding_events"] = static_cast<Json::UInt64>(stats_.forwarding_events);
    stats["deadline_adjustments"] = static_cast<Json::UInt64>(stats_.deadline_adjustments);
    stats["avg_frame_completion_time"] = stats_.avg_frame_completion_time;
    stats["avg_frame_deadline"] = stats_.avg_frame_deadline;
    stats["avg_jitter"] = stats_.avg_jitter;
    
    // Add active frames
    Json::Value frames(Json::arrayValue);
    for (const auto& frame_pair : active_frames_) {
        const FrameStatus& frame = frame_pair.second;
        
        Json::Value frame_json;
        frame_json["timestamp"] = static_cast<Json::UInt>(frame.timestamp);
        frame_json["progress"] = frame.progress;
        frame_json["deadline_ms"] = static_cast<Json::Int>(frame.deadline.count());
        frame_json["assigned_link"] = frame.assigned_link;
        
        frames.append(frame_json);
    }
    stats["active_frames"] = frames;
    
    // Add link metrics
    Json::Value links(Json::objectValue);
    for (const auto& link_pair : link_metrics_) {
        const LinkMetrics& metrics = link_pair.second;
        
        Json::Value link_json;
        link_json["throughput_mbps"] = metrics.throughput_mbps;
        link_json["delay_ms"] = metrics.delay_ms;
        link_json["jitter_ms"] = metrics.jitter_ms;
        link_json["packet_loss"] = metrics.packet_loss;
        link_json["buffer_occupancy_ratio"] = metrics.buffer_occupancy_ratio;
        link_json["is_congested"] = metrics.is_congested;
        
        // Check if forwarding is enabled for this link
        auto fwd_it = forwarding_enabled_.find(link_pair.first);
        bool is_forwarding_enabled = (fwd_it != forwarding_enabled_.end()) ? fwd_it->second : false;
        link_json["forwarding_enabled"] = is_forwarding_enabled;
        
        links[std::to_string(link_pair.first)] = link_json;
    }
    stats["link_metrics"] = links;
    
    // Add QoE metrics if available
    if (qoe_processor_) {
        stats["qoe_metrics"] = qoe_processor_->getQoEMetrics();
        stats["jitter_stats"] = qoe_processor_->getJitterStats();
    }
    
    // Add multilink scheduling information
    Json::Value scheduling(Json::objectValue);
    for (const auto& schedule_pair : frame_scheduling_) {
        const FrameSchedulingInfo& info = schedule_pair.second;
        Json::Value frame_info;
        
        frame_info["timestamp"] = static_cast<Json::UInt>(info.timestamp);
        frame_info["total_size_kb"] = info.total_size_kb;
        
        // Add link assignments
        Json::Value assignments(Json::arrayValue);
        for (const auto& assignment : info.link_assignments) {
            Json::Value assign_info;
            assign_info["link_id"] = assignment.link_id;
            assign_info["chunk_size_kb"] = assignment.chunk_size_kb;
            assign_info["expected_completion_time_ms"] = assignment.expected_completion_time_ms;
            assignments.append(assign_info);
        }
        
        frame_info["assignments"] = assignments;
        
        // Time since scheduling
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - info.schedule_time);
        frame_info["elapsed_since_schedule_ms"] = static_cast<Json::UInt64>(elapsed.count());
        
        scheduling[std::to_string(info.timestamp)] = frame_info;
    }
    
    stats["multilink_scheduling"] = scheduling;
    
    return stats;
}

void QconScheduler::setupLogFiles() {
    if (log_dir_.empty()) {
        LOG_MODULE_ERROR(MODULE, "Cannot setup log files: log directory not set");
        return;
    }
    
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    // Initialize QoE log file
    std::string qoe_log_path = log_dir_ + "/qoe_metrics.csv";
    qoe_logger_ = std::make_unique<AsyncLogger>(qoe_log_path);
    qoe_logger_->logLine("timestamp_ms,frame_ts,completion_time_ms,avg_bitrate,avg_jitter,"
                   "avg_frame_rate,packet_loss,avg_frame_completion_time");
    LOG_MODULE_INFO(MODULE, "QoE metrics will be logged to " << qoe_log_path);

    // Initialize J_on adaptive-jitter log
    std::string j_on_log_path = log_dir_ + "/j_on_log.csv";
    j_on_logger_ = std::make_unique<AsyncLogger>(j_on_log_path);
    j_on_logger_->logLine("timestamp_ms,j_on_ms,j_off_ms,j_th_used_ms,"
                          "bitrate_kbps,prev_bitrate_kbps,bitrate_drop_pct,"
                          "current_jitter_ms,action,deadline_ms");

    // Initialize re-injection decision log (paper §5.4 priority-based re-inject)
    std::string reinject_log_path = log_dir_ + "/reinject_log.csv";
    reinject_logger_ = std::make_unique<AsyncLogger>(reinject_log_path);
    reinject_logger_->logLine("timestamp_ms,frame_size_kb,deadline_ms,"
                              "current_link,current_bw_mbps,current_ect_ms,"
                              "alt_link,alt_bw_mbps,alt_ect_ms,"
                              "slack_ms,trigger,decision");
    
    // Initialize scheduling log file
    std::string scheduling_log_path = log_dir_ + "/scheduling_decisions.csv";
    scheduling_logger_ = std::make_unique<AsyncLogger>(scheduling_log_path);
    scheduling_logger_->logLine("timestamp_ms,frame_ts,selected_link,requires_path_change,"
                         "decision_reason,best_completion_time,estimated_frame_size_kb,deadline,multilink_used,"
                         "total_cost,selected_links,chunk_sizes,split_ratio");
    LOG_MODULE_INFO(MODULE, "Scheduling decisions will be logged to " << scheduling_log_path);
    
    // Initialize deadline log file
    std::string deadline_log_path = log_dir_ + "/deadline_adjustments.csv";
    deadline_logger_ = std::make_unique<AsyncLogger>(deadline_log_path);
    deadline_logger_->logLine("timestamp_ms,frame_ts,progress,remaining_bytes,expected_time,"
                        "original_deadline,adjusted_deadline,avg_jitter,jitter_threshold,"
                        "adjustment_factor,adjustment_applied,first_arrival_time_ms,"
                        "prev_frame_jitter");
    LOG_MODULE_INFO(MODULE, "Deadline adjustments will be logged to " << deadline_log_path);
    
    // Initialize frame lifecycle log
    std::string frame_lifecycle_path = log_dir_ + "/frame_lifecycle.csv";
    frame_lifecycle_logger_ = std::make_unique<AsyncLogger>(frame_lifecycle_path);
    frame_lifecycle_logger_->logLine("timestamp_ms,frame_ts,event,progress,estimated_size_kb,actual_size_kb,"
                               "assigned_link,expected_completion_ms,actual_completion_ms,"
                               "deadline_ms,is_multilink,first_arrival_time_ms,marker_received_time_ms,"
                               "link_ids,chunk_sizes_kb,throughput_mbps,rtt_ms");
    LOG_MODULE_INFO(MODULE, "Frame lifecycle will be logged to " << frame_lifecycle_path);
}

SchedulingDecision QconScheduler::makeDecisionInternal() {
    LOG_MODULE_DEBUG(MODULE, "Making QCON scheduling decision");

    uint32_t most_urgent_frame = qoe_processor_->recent_timestamp_;

    // Make decision for the most urgent frame
    SchedulingDecision decision;
    decision.requires_path_change = false;

    // Declare these variables in the outer scope so they're available for logging later
    std::vector<int> selected_link_ids;
    std::vector<double> selected_chunk_sizes;
    double total_cost = 0.0;
    bool multilink_used = false;
    
    //FrameStatus& frame = frame_it->second;

    // Paper §5.3 eq 3: D_m = min(D_max, T_{m-1} + (t_m - t_{m-1}) + J_th)
    //   J_th = max(J_off, J_on)
    // Override J_off via QCON_J_OFF_MS (default 30); set =0 to disable adaptive
    // deadline behaviour (Fig 19 fixed-deadline ablation).
    static const double j_off_env = []{
        const char* e = std::getenv("QCON_J_OFF_MS");
        double v = 30.0;
        if (e) { try { v = std::stod(e); } catch (...) {} }
        return v;
    }();
    j_off_ms_ = j_off_env;
    double deadline_ms = app_deadline_;  // default = D_max
    double current_bitrate_kbps = 0.0;
    double current_jitter_ms    = 0.0;
    double current_fps          = 0.0;
    int64_t this_frame_arrival_ms = 0;
    if (qoe_processor_) {
        current_bitrate_kbps = qoe_processor_->getQoEMetrics()["avg_bitrate"].asDouble();
        current_jitter_ms    = qoe_processor_->getQoEMetrics()["avg_jitter"].asDouble();
        current_fps          = qoe_processor_->getRecentFrameRateFps(100);

        // Paper §5.3 J_on update: J_on = MEASURED max frame jitter over recent
        // window. Earlier impl used a counter ramp/jump rule that maxed out at
        // J_off, so J_th = max(J_on, J_off) was effectively constant. Now J_on
        // tracks observed inter-frame jitter so J_th grows when network is
        // jittery and stays at J_off when smooth.
        auto now = std::chrono::steady_clock::now();
        if (last_j_on_update_.time_since_epoch().count() == 0) {
            last_j_on_update_ = now;
            last_window_bitrate_kbps_ = current_bitrate_kbps;
        }
        auto since_update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_j_on_update_).count();
        std::string j_on_action = "hold";
        if (since_update_ms >= 500) {
            prev_window_bitrate_kbps_ = last_window_bitrate_kbps_;
            last_window_bitrate_kbps_ = current_bitrate_kbps;
            last_window_jitter_ms_    = current_jitter_ms;

            double drop_pct = 0.0;
            if (prev_window_bitrate_kbps_ > 0) {
                drop_pct = (prev_window_bitrate_kbps_ - current_bitrate_kbps)
                           / prev_window_bitrate_kbps_;
            }
            // Measured jitter from recent completed frames.
            // Window 60 frames (2s @ 30fps) — wider window catches recent BWE
            // collapses that 1-second window misses (jitter spike receded
            // before we sampled). Stable traces don't change much.
            double measured_j_on = qoe_processor_->getRecentMaxFrameJitterMs(60);
            double prev_j_on = j_on_ms_;
            // Leaky-max smoothing: rise instantly (spike caught), fall slowly
            // (decay 0.95/tick ≈ 1s half-life). Without this, J_on drops the
            // moment the jitter recedes — scheduler's deadline snaps tight
            // again → reinject burst fires → BWE noise → collapse oscillation.
            // With leaky decay J_on stays elevated long enough for BWE to
            // genuinely recover.
            if (measured_j_on >= j_on_ms_) {
                j_on_ms_ = measured_j_on;             // rise: instant
            } else {
                j_on_ms_ = j_on_ms_ * 0.95 + measured_j_on * 0.05;  // fall: slow
            }
            if (j_on_ms_ > prev_j_on + 5.0)         j_on_action = "rise";
            else if (j_on_ms_ < prev_j_on - 5.0)    j_on_action = "fall";
            else                                    j_on_action = "stable";
            last_j_on_update_ = now;

            if (j_on_logger_) {
                std::ostringstream line;
                line << std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() << ","
                     << j_on_ms_ << "," << j_off_ms_ << ","
                     << std::max(j_on_ms_, j_off_ms_) << ","
                     << current_bitrate_kbps << "," << prev_window_bitrate_kbps_ << ","
                     << drop_pct * 100.0 << "," << current_jitter_ms << ","
                     << j_on_action << "," << deadline_ms;
                j_on_logger_->logLine(line.str());
            }
        }

        // Paper §5.3 eq3: D_m = T_{m-1} + (t_m - t_{m-1}) + J_th
        // T_{m-1} = SINGLE most-recent completed frame's delivery time, NOT
        // a sliding-window average. Previously we read avg_frame_completion_time
        // which never tightened deadline when frames arrived fast.
        this_frame_arrival_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        double last_delivery_ms = qoe_processor_->getLastCompletedFrameDeliveryMs();
        if (last_delivery_ms > 0.0) prev_frame_completion_ms_ = last_delivery_ms;

        double j_th = std::max(j_on_ms_, j_off_ms_);
        if (prev_frame_arrival_ms_ > 0) {
            int64_t inter_arrival_ms = this_frame_arrival_ms - prev_frame_arrival_ms_;
            double eq3 = prev_frame_completion_ms_ + static_cast<double>(inter_arrival_ms) + j_th;
            // Paper §5.3 strict: D_max = app_deadline_ (hard latency cap).
            // J_on grows toward D_max when network jittery → D_m approaches
            // D_max → scheduler is RELAXED (more time, less aggressive about
            // multi-link / reinject). When BWE is struggling, this naturally
            // throttles disruptive path-switching that would compound the
            // BWE confusion. The deadline relief comes from J_on growing
            // INSIDE D_max, not from raising D_max.
            deadline_ms = std::min<double>(static_cast<double>(app_deadline_), eq3);
        }
        prev_frame_arrival_ms_ = this_frame_arrival_ms;

        // Log raw QoE metrics on every scheduling decision so we can debug
        // what the scheduler is actually seeing.
        // Header: timestamp_ms,frame_ts,completion_time_ms,avg_bitrate,avg_jitter,
        //         avg_frame_rate,packet_loss,avg_frame_completion_time
        if (qoe_logger_) {
            double pkt_loss = qoe_processor_->getQoEMetrics()["packet_loss"].asDouble();
            std::ostringstream qline;
            qline << this_frame_arrival_ms << ","
                  << most_urgent_frame << ","
                  << prev_frame_completion_ms_ << ","
                  << current_bitrate_kbps << ","
                  << current_jitter_ms << ","
                  << current_fps << ","
                  << pkt_loss << ","
                  << prev_frame_completion_ms_;
            qoe_logger_->logLine(qline.str());
        }
    }
    
    // Estimate frame size. Realistic default for 1080p/60fps/25Mbps video ≈ 52 KB/frame.
    // Live QoE bitrate/framerate overrides when available; otherwise use last-known
    // or a 50 KB default. Prevents the degenerate "1 KB frame trivially fits any link"
    // case that otherwise hides real link congestion from QCON's optimizer.
    double default_frame_kb = 50.0;
    if (const char* env = std::getenv("QCON_DEFAULT_FRAME_KB")) {
        try { default_frame_kb = std::stod(env); } catch(...) {}
    }
    // Paper §5.3: f_m = average frame size from a 100 ms sliding window.
    // Primary path uses getRecentFrameSizeBytes which is populated by BPF as packets
    // arrive (does NOT depend on the brittle RLC-ack frame completion pipeline).
    // Falls back to the bitrate/fps quotient (also 100 ms window inside QoEProcessor)
    // when arrival-window is empty (very first frames), and finally the env default.
    double frame_size_kb = default_frame_kb;
    if (qoe_processor_) {
        double size_bytes = qoe_processor_->getRecentFrameSizeBytes(100);
        if (size_bytes > 0) {
            frame_size_kb = size_bytes / 1024.0;
            prev_frame_size_kb_ = frame_size_kb;
        } else {
            double bitrate_kbps = qoe_processor_->getQoEMetrics()["avg_bitrate"].asDouble();
            double framerate    = qoe_processor_->getQoEMetrics()["avg_frame_rate"].asDouble();
            if (bitrate_kbps > 0 && framerate > 0) {
                frame_size_kb = bitrate_kbps / (8.0 * framerate);
                prev_frame_size_kb_ = frame_size_kb;
            } else if (prev_frame_size_kb_ > 1) {
                frame_size_kb = prev_frame_size_kb_;
            }
        }
    }

    // Use raw measured BW. The PHY/DU layer reports the link's current capacity
    // directly from the trace (pdcp_du_link.cpp:484 → phy->getCurrentBandwidth),
    // so measured=0 truthfully means the radio is down right now and we must
    // route to the other link. NEVER-measured links (bootstrap window before
    // trace starts) are EXCLUDED entirely so the scheduler defers instead of
    // emitting a wasteful 50/50 best-effort multilink fallback that locks the
    // data plane onto the wrong path before real BW arrives.
    std::vector<std::pair<int, LinkMetrics>> available_links;
    for (const auto& link_pair : link_metrics_) {
        int link_id = link_pair.first;
        LinkMetrics metrics = link_pair.second;
        double measured = metrics.throughput_mbps;
        if (measured > 0.0) {
            last_known_bw_mbps_[link_id] = measured;
            available_links.push_back({link_id, metrics});
        } else if (last_known_bw_mbps_.find(link_id) != last_known_bw_mbps_.end()) {
            // Measured before but currently 0 — keep at 0 so dropout surfaces
            // as the trigger it is (alg-1 will pick the other link).
            available_links.push_back({link_id, metrics});
        }
        // else: never measured → skip; scheduler defers via "No active links".
    }

    
    // Sort links by cost (assuming link_id is the cost)
    std::sort(available_links.begin(), available_links.end(), 
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Try single-link solutions first (lower total cost)
    bool deadline_met = false;
    int selected_link = 0;
    double best_completion_time = std::numeric_limits<double>::max();


    if (available_links.empty()) {
        decision = current_decision_;
        selected_link = current_decision_.selected_link_id;
        decision.decision_reason = "No active links – scheduling deferred";
        best_completion_time = 10000;
    }
    
    // Get last frame completion time for deadline setting
    double last_frame_completion_time_ms = app_deadline_; // Default to 33ms (~30fps)
    if (!frame_completion_times_.empty()) {
        auto temp_queue = frame_completion_times_;
        last_frame_completion_time_ms = temp_queue.back().count();
    } else if (stats_.avg_frame_completion_time > 0) {
        last_frame_completion_time_ms = stats_.avg_frame_completion_time;
    }
    
    // Convert our link metrics to the format expected by DeadlineSettings.
    // Use available_links (sticky-floored bandwidths) instead of raw link_metrics_:
    // when an inactive link has measured BW=0, the floor lets calculateExpectedDeliveryTime
    // see the last-known good value rather than infinite completion time, which otherwise
    // permanently locked single-link selection to whichever link was first-active.
    std::map<int, double> link_bandwidths;
    std::map<int, double> link_latencies;
    std::map<int, double> link_buffer_bytes;

    for (const auto& link_pair : available_links) {
        int link_id = link_pair.first;
        const LinkMetrics& metrics = link_pair.second;

        link_bandwidths[link_id] = metrics.throughput_mbps;
        link_latencies[link_id] = 0;
        link_buffer_bytes[link_id] = static_cast<double>(metrics.queue_size_bytes);
    }
    
    // Rate-limit path switches: after each change, hold off further switches for
    // a min interval. Algorithm 1's cost-ascending verdict is honoured (we still
    // pick cheapest meeting deadline), but a switch decision must wait until the
    // hold-off has elapsed — otherwise we stay on the previously committed link
    // even if cost-ascending would prefer something different right now. This
    // gives WebRTC's CCA on the single tun_host enough time to ramp up between
    // changes, instead of being reset every 100-300 ms by alg-1 fluctuation.
    // Disable with QCON_HYSTERESIS=0; tune hold-off via QCON_PATH_HOLDOFF_MS
    // (default 500 ms — empirically enough for BWE to make a couple of round-trips).
    static const bool hysteresis_on = []{
        const char* e = std::getenv("QCON_HYSTERESIS");
        return !(e && std::string(e) == "0");
    }();
    static const int64_t holdoff_ms = []{
        const char* e = std::getenv("QCON_PATH_HOLDOFF_MS");
        // Default 1000ms — empirically the sweet spot on real traces.
        // 500ms: too volatile, BWE GCC randomly collapses on rapid 5G dips
        //        (att_city/verizon: median 6509 / 16957).
        // 1000ms: median 10641 / 19665 / 22939 (att/verizon/tmobile, +7% vs Single
        //         on tmobile, ≈Ideal on tmobile, +0.5% vs Single on verizon).
        // 2000ms: over-conservative, drops back to ~ unimproved levels.
        int64_t v = 1000;
        if (e) { try { v = std::stoll(e); } catch (...) {} }
        return v;
    }();
    int64_t now_ms_for_holdoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // First pass: try each link individually (lowest cost)
    if (!deadline_met) for (const auto& link_pair : available_links) {
        int link_id = link_pair.first;
        const LinkMetrics& metrics = link_pair.second;

        std::map<int, double> chunk = { { link_id, frame_size_kb } };

        double completion_time_ms = deadline_settings_.calculateExpectedDeliveryTime(
            chunk,
            link_bandwidths,
            link_latencies,
            link_buffer_bytes
        );

        LOG_MODULE_WARN(MODULE, "Link " << link_id << " calculated delivery time: "
                    << completion_time_ms << "ms (deadline: " << deadline_ms << "ms)");
        selected_link = link_id - 1;
        total_cost = link_id;

        best_completion_time = std::min(best_completion_time, completion_time_ms);
        decision.selected_link_id = selected_link;
        decision.requires_path_change = (selected_link != current_link_id_);

        // Strict paper Algorithm 1: single-link winner takes 100% of frame.
        // (Backup-link primer experiment with 5% split was net-negative —
        // multilink mode overhead degraded transport faster than CCA gains.)
        decision.split_ratio = { { selected_link, 1.0 } };

        if (decision.requires_path_change) {
            decision.decision_reason = "Single link scheduling for frame " + std::to_string(selected_link);
        } else {
            decision.decision_reason = "Maintaining single link " + std::to_string(selected_link);
        }

        // Check if this link can meet the deadline
        if (completion_time_ms <= deadline_ms) {
            deadline_met = true;
            break;
        }
    }

    // Asymmetric holdoff (M12). Cost-ascending optimality is preserved on the
    // CHEAP-DIRECTION switch (e.g. 4G→5G): always commit immediately so we
    // pick up free 5G capacity ASAP.  Only the EXPENSIVE-DIRECTION switch
    // (5G→4G) is rate-limited, because that's the one that, if done too often,
    // ends up parking on the expensive link and never coming back (the M11
    // failure mode: once on 4G with 80 Mbps avg, 4G always meets deadline so
    // the symmetric hysteresis kept locking us there even when 5G was fine).
    bool switch_to_cheaper =
        decision.requires_path_change && current_link_id_ >= 0 &&
        (selected_link + 1) < (current_link_id_ + 1);  // 1-indexed cost
    bool switch_to_expensive =
        decision.requires_path_change && current_link_id_ >= 0 &&
        (selected_link + 1) > (current_link_id_ + 1);

    if (hysteresis_on && deadline_met && switch_to_expensive &&
        (now_ms_for_holdoff - last_path_change_ms_) < holdoff_ms) {
        int cur_one_indexed = current_link_id_ + 1;
        auto cur_it = std::find_if(available_links.begin(), available_links.end(),
            [&](const auto& p){ return p.first == cur_one_indexed; });
        if (cur_it != available_links.end()) {
            std::map<int, double> chunk = { { cur_one_indexed, frame_size_kb } };
            double cur_completion_ms = deadline_settings_.calculateExpectedDeliveryTime(
                chunk, link_bandwidths, link_latencies, link_buffer_bytes);
            if (cur_completion_ms <= deadline_ms) {
                selected_link = current_link_id_;
                total_cost = cur_one_indexed;
                best_completion_time = cur_completion_ms;
                decision.selected_link_id = selected_link;
                decision.requires_path_change = false;
                decision.split_ratio = { { selected_link, 1.0 } };
                decision.decision_reason = "Holdoff (expensive): keep "
                    + std::to_string(selected_link);
            }
        }
    }
    (void)switch_to_cheaper;
    if (decision.requires_path_change) {
        last_path_change_ms_ = now_ms_for_holdoff;
    }

    // If no single link can meet the deadline, try multilink combinations.
    // QCON_DISABLE_MULTILINK=1 skips this branch (ablation control).
    static const bool multilink_off = []{
        const char* e = std::getenv("QCON_DISABLE_MULTILINK");
        return (e && std::string(e) == "1");
    }();
    // Paper Algorithm 1 strict: multilink ONLY when single FAILS deadline.
    // Use 4G as little as possible — paper-correct cost-ascending.
    if (!multilink_off && !deadline_met && available_links.size() > 1) {
        // Check if we should use multilink based on expected delivery time and deadline
        bool should_use_multilink = true;

        if (should_use_multilink) {
            LOG_MODULE_INFO(MODULE, "Should use multilink: best_completion_time="
                        << best_completion_time << "ms exceeds "
                        << qcon_config_.deadline_threshold * 100 << "% of deadline="
                        << deadline_ms << "ms");
            
            // Calculate optimal multilink distribution with minimum cost
            std::vector<int> selected_link_ids;
            std::vector<double> selected_chunk_sizes;
            double total_cost = 0;
            double multilink_completion_time = std::numeric_limits<double>::max();
            
            // Find minimum cost multilink solution and get the best completion time
            // Even if deadline isn't met, we'll get the best completion time set
            findMinCostMultilinkSolution(
                frame_size_kb, deadline_ms, selected_link_ids, selected_chunk_sizes, 
                total_cost, multilink_completion_time);

            // completion time, selected link ids, and selected chunk sizes log
            LOG_MODULE_DEBUG(MODULE, "Multilink solution: completion_time=" 
                        << multilink_completion_time << "ms, selected_links=" 
                        << selected_link_ids.size() << ", total_cost=" 
                        << total_cost << ", chunk_sizes=" 
                        << selected_chunk_sizes.size());
            
            for (size_t i = 0; i < selected_chunk_sizes.size(); ++i) {
                LOG_MODULE_DEBUG(MODULE, "Chunk size for link " 
                            << selected_link_ids[i] << ": " 
                            << selected_chunk_sizes[i] << " KB");
            }
            
            // Use the multilink solution if we got a valid result back (links were selected)
            if (!selected_link_ids.empty() && multilink_completion_time < std::numeric_limits<double>::max()) {
                // Create link assignments based on the solution
                std::vector<LinkAssignment> link_assignments;
                
                // Calculate total chunk size across all links
                double total_chunk_size = 0.0;
                for (size_t i = 0; i < selected_chunk_sizes.size(); i++) {
                    total_chunk_size += selected_chunk_sizes[i];
                }

                // Create the decision
                int primary_link = selected_link_ids[0];
                decision.selected_link_id = primary_link;
                decision.is_multipath = (selected_link_ids.size() > 1);
                // requires_path_change must capture single↔multilink TRANSITIONS
                // even when primary_link stayed the same. Without this, a sustained
                // multilink decision (primary=0, current=0) sees requires=false →
                // sendMultilinkCommandImmediate is skipped → sender never enters
                // multilink_active_ → all bytes pile on active_path (link 0).
                decision.requires_path_change =
                    (primary_link != current_link_id_) ||
                    (decision.is_multipath != current_decision_.is_multipath);

                // Calculate and add split ratios for each link
                for (size_t i = 0; i < selected_link_ids.size(); i++) {
                    int link_id = selected_link_ids[i] - 1;
                    
                    // Calculate split ratio
                    double ratio = 0.0;
                    if (total_chunk_size > 0) {
                        ratio = selected_chunk_sizes[i] / total_chunk_size;
                    }
                    
                    // Add to the decision's split_ratio map
                    decision.split_ratio[link_id] = ratio;
                }
                current_decision_ = decision;
                
                // Add multilink assignment details as a JSON string
                Json::Value assignments_json = getSchedulingDecisionJson(link_assignments);
                Json::FastWriter writer;
                std::string assignments_str = writer.write(assignments_json);
                
                if (multilink_completion_time <= deadline_ms) {
                    if (decision.requires_path_change) {
                        decision.decision_reason = "Min-cost multilink scheduling for frame";
                    } else {
                        decision.decision_reason = "Maintaining primary link";
                    }
                    deadline_met = true;
                } else {
                    // We're using multilink even though deadline isn't met because it's the best option
                    decision.decision_reason = "Best-effort multilink scheduling (deadline not met)";
                    LOG_MODULE_DEBUG(MODULE, "Using best multilink solution with completion time=" 
                                << multilink_completion_time << "ms (deadline=" << deadline_ms << "ms)");
                }
                
                // Update best_completion_time and mark that we've handled the scheduling
                best_completion_time = multilink_completion_time;
                deadline_met = true; // Skip the single-link fallback
            }
        }
    }

    
    // Check for packet forwarding
    /*
    if (qcon_config_.enable_packet_forwarding) {
        for (const auto& link_pair : link_metrics_) {
            int link_id = link_pair.first;
            
            if (shouldForwardPackets(link_id)) {
                // Trigger packet forwarding for this link
                forwarding_enabled_[link_id] = true;
                stats_.forwarding_events++;
                
                LOG_MODULE_INFO(MODULE, "Enabling packet forwarding for link " << link_id);
            } else {
                forwarding_enabled_[link_id] = false;
            }
        }
    }
    */
    
    // Log scheduling decision to CSV
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (scheduling_logger_) {
            auto now = std::chrono::system_clock::now();
            // Get milliseconds since epoch for timestamp
            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            std::string timestamp_str = std::to_string(ms_since_epoch);
            
            // Get frame deadline if available
            std::string frame_deadline = std::to_string(deadline_ms);
            std::string frame_ts = std::to_string(most_urgent_frame);
            
            // Was multilink used? Read decision.is_multipath / split_ratio directly.
            // (Earlier impl checked frame_scheduling_ which isn't populated by the
            // multilink fallback path, so it always returned false even when the
            // scheduler actually emitted a multilink decision.)
            // multilink_used is "effective multilink": split_ratio has ≥2 entries
            // AND each entry contributes a non-trivial share. Earlier we counted
            // {0:0.0, 1:1.0} as multilink because the map had 2 entries — that
            // overcounts: a pseudo-split where one side is 0% is a single-link
            // decision with extra noise in the log. Require both sides ≥5%.
            int effective_links = 0;
            for (const auto& kv : decision.split_ratio) {
                if (kv.second >= 0.05) ++effective_links;
            }
            bool multilink_used = decision.is_multipath && effective_links >= 2;
            
            // Get selected links and chunk sizes for multilink
            std::string selected_links = "";
            std::string chunk_sizes = "";
            std::string split_ratio = "";
            
            if (multilink_used && !selected_link_ids.empty()) {
                for (size_t i = 0; i < selected_link_ids.size(); i++) {
                    selected_links += std::to_string(selected_link_ids[i]);
                    if (i < selected_link_ids.size() - 1) selected_links += "|";
                }
                
                for (size_t i = 0; i < selected_chunk_sizes.size(); i++) {
                    chunk_sizes += std::to_string(selected_chunk_sizes[i]);
                    if (i < selected_chunk_sizes.size() - 1) chunk_sizes += "|";
                }
            }
            
            // Format split ratio information
            for (const auto& ratio_pair : decision.split_ratio) {
                if (!split_ratio.empty()) split_ratio += "|";
                split_ratio += std::to_string(ratio_pair.first) + ":" + std::to_string(ratio_pair.second);
            }
            
            // Ensure we don't have NaN or inf values in the output
            std::string formatted_completion_time = std::to_string(best_completion_time);
            std::string formatted_frame_size = std::to_string(frame_size_kb);
            
            // Replace NaN and inf values with valid strings
            if (std::isnan(best_completion_time)) {
                formatted_completion_time = "N/A";
            } else if (std::isinf(best_completion_time)) {
                formatted_completion_time = "Infinity";
            }
            
            if (std::isnan(frame_size_kb)) {
                formatted_frame_size = "N/A";
            } else if (std::isinf(frame_size_kb)) {
                formatted_frame_size = "Infinity";
            }
            
            // Create CSV line
            std::string log_line = 
                timestamp_str + "," +
                frame_ts + "," +
                std::to_string(decision.selected_link_id) + "," +
                (decision.requires_path_change ? "yes" : "no") + "," +
                "\"" + decision.decision_reason + "\"" + "," +
                formatted_completion_time + "," +
                formatted_frame_size + "," +
                frame_deadline + "," +
                (multilink_used ? "yes" : "no") + "," +
                std::to_string(total_cost) + "," +
                "\"" + selected_links + "\"" + "," +
                "\"" + chunk_sizes + "\"" + "," +
                "\"" + split_ratio + "\"";
                
            scheduling_logger_->logLine(log_line);
        }
    }

    current_link_id_ = decision.selected_link_id;
    current_decision_ = decision;
    qoe_processor_->saveSchedulingInfo(current_decision_.split_ratio);

    LOG_MODULE_DEBUG(MODULE, "Scheduling decision made: selected link "
                << decision.selected_link_id << ", requires path change: "
                << (decision.requires_path_change ? "yes" : "no") << ", reason: "
                << decision.decision_reason);

    // Paper §5.4 — priority-based re-injection: after the main decision is made,
    // check whether the chosen single-link is at risk of missing its deadline AND
    // an alt link could absorb the work. Pass RAW link_metrics_ (not sticky-floored
    // available_links) so genuine dropouts (measured BW=0) yield infinite ECT and
    // correctly trigger re-injection. Use sticky for the alt-link's capacity check
    // so we still know which alt is viable.
    if (!decision.is_multipath && link_metrics_.size() >= 2) {
        // Build raw + sticky-fallback pair vector (raw for current, sticky for alt).
        std::vector<std::pair<int, LinkMetrics>> raw_links;
        for (const auto& [lid, m] : link_metrics_) raw_links.push_back({lid, m});
        if (evaluateReinjection(decision, frame_size_kb, deadline_ms, raw_links)) {
            // Re-injection fired — decision was mutated to alt link. Update sticky state
            // so that subsequent ticks honor the new active link.
            current_link_id_ = decision.selected_link_id;
            current_decision_ = decision;
        }

        // In-flight frame sweep — runs ALWAYS (cheap, side-effects:
        // promote frames to COMPLETE/TIMEOUT so frame_delays gets fed; that
        // queue feeds T_{m-1} and J_on per paper §5.3 — without it both stay
        // stuck at defaults). What's GATED is the actual reinject burst.
        // QCON_DISABLE_REINJECT_BURST=1 (default) keeps the burst quiet so
        // WebRTC GCC isn't confused by per-frame packet duplication on rapidly
        // flapping real traces. Set to 0 only on microbench traces where the
        // burst provably helped (reinject_trigger long dropout).
        static const bool burst_disabled = []{
            const char* e = std::getenv("QCON_DISABLE_REINJECT_BURST");
            // default: TRUE (burst off). Old QCON_ENABLE_INFLIGHT_SWEEP=1 still
            // accepted (legacy) — that env forces burst ON.
            const char* legacy = std::getenv("QCON_ENABLE_INFLIGHT_SWEEP");
            if (legacy && std::string(legacy) == "1") return false;  // legacy ON ⇒ burst ON
            if (e && std::string(e) == "0") return false;             // explicit ON
            return true;                                              // default OFF
        }();
        if (qoe_processor_ && controller_) {
            uint32_t newest_ts = qoe_processor_->getNewestFrameTs();
            auto candidates = qoe_processor_->sweepInProgressFrames(
                newest_ts, static_cast<int64_t>(app_deadline_));

            int  cur_zero  = current_decision_.selected_link_id;
            int  alt_zero  = -1;
            double alt_bw  = 0.0;
            for (const auto& [lid_one, m] : link_metrics_) {
                int lid = lid_one - 1;
                if (lid == cur_zero) continue;
                if (alt_zero < 0 || m.throughput_mbps > alt_bw) {
                    alt_zero = lid;
                    alt_bw   = m.throughput_mbps;
                }
            }

            // Burst-fire gate: respects burst_disabled (default true). Legacy
            // QCON_DISABLE_REINJECT=1 still works as a kill switch.
            static const bool reinject_off2 = []{
                const char* e = std::getenv("QCON_DISABLE_REINJECT");
                return (e && std::string(e) == "1");
            }();
            const bool burst_blocked = burst_disabled || reinject_off2;

            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            for (const auto& c : candidates) {
                // Look up the cur link's BW for this frame (its primary link).
                double cur_bw = 0.0;
                auto it_cur = link_metrics_.find(c.primary_link_id + 1);
                if (it_cur != link_metrics_.end()) cur_bw = it_cur->second.throughput_mbps;

                double remaining_ect_cur_ms = (cur_bw > 0.0)
                    ? (c.unacked_bytes * 8.0) / cur_bw / 1000.0
                    : std::numeric_limits<double>::infinity();
                double remaining_ect_alt_ms = (alt_bw > 0.0)
                    ? (c.unacked_bytes * 8.0) / alt_bw / 1000.0
                    : std::numeric_limits<double>::infinity();
                double slack = static_cast<double>(app_deadline_) - c.elapsed_ms;
                bool   past_deadline = (c.elapsed_ms > static_cast<int64_t>(app_deadline_));
                // Will current link miss this frame's deadline?
                bool   will_miss = past_deadline
                                || (cur_bw <= 0.0)
                                || (remaining_ect_cur_ms > slack);
                bool   alt_ok    = (alt_zero >= 0) && (alt_bw > 0.0)
                                && (remaining_ect_alt_ms <= std::max(0.0, slack)
                                    || past_deadline);

                std::string action;
                bool fire = false;
                if (c.already_reinjected)        action = "inflight_already";
                else if (!will_miss)             action = "inflight_ok";
                else if (!alt_ok)                action = past_deadline
                                                       ? "inflight_zombie_no_alt"
                                                       : "inflight_preempt_no_alt";
                else {
                    fire = true;
                    action = past_deadline ? "inflight_zombie" : "inflight_preempt";
                }

                if (reinject_logger_) {
                    std::ostringstream line;
                    line << now_ms << "," << (c.unacked_bytes/1024.0) << ","
                         << app_deadline_ << ","
                         << c.primary_link_id << "," << cur_bw << ","
                         << remaining_ect_cur_ms << ","
                         << alt_zero << "," << alt_bw << ","
                         << remaining_ect_alt_ms << ","
                         << slack << "," << (will_miss ? "yes" : "no") << ","
                         << action;
                    reinject_logger_->logLine(line.str());
                }

                if (fire && !burst_blocked) {
                    int burst_pkts = std::max<int>(1,
                        static_cast<int>((c.unacked_bytes + 1499) / 1500));
                    burst_pkts = std::min(burst_pkts, 60);
                    controller_->sendReinjectBurst(alt_zero, burst_pkts);
                    qoe_processor_->markFrameReinjected(c.rtp_ts);
                }
            }
        }
    }

    return decision;
}

bool QconScheduler::evaluateReinjection(SchedulingDecision& decision,
                                        double frame_size_kb,
                                        double deadline_ms,
                                        const std::vector<std::pair<int, LinkMetrics>>& links) {
    // Find the currently selected link's BW and an alternate.
    int cur = current_decision_.selected_link_id;
    double cur_bw = 0.0, alt_bw = 0.0;
    int alt = -1;
    for (const auto& [lid, m] : links) {
        // available_links keys are 1-indexed; selected_link_id is 0-indexed (paper convention).
        int zero_idx = lid - 1;
        if (zero_idx == cur) cur_bw = m.throughput_mbps;
        else if (alt < 0 || m.throughput_mbps > alt_bw) {
            alt = zero_idx;
            alt_bw = m.throughput_mbps;
        }
    }
    // Apply sticky fallback for ALT link only (so we have a viable target). Current
    // link uses raw measurement so dropouts surface as the trigger they are.
    if (alt >= 0 && alt_bw <= 0.0) {
        auto it = last_known_bw_mbps_.find(alt + 1);  // last_known map is 1-indexed
        if (it != last_known_bw_mbps_.end() && it->second > 0.0) alt_bw = it->second;
    }
    if (alt < 0) return false;
    // current_ect: cur_bw=0 → effectively infinite (force trigger when current link dead)
    double current_ect = (cur_bw > 0.0)
        ? (frame_size_kb * 8.0) / cur_bw
        : std::numeric_limits<double>::infinity();
    double alt_ect = (alt_bw > 0.0)
        ? (frame_size_kb * 8.0) / alt_bw
        : std::numeric_limits<double>::infinity();
    double slack       = deadline_ms;  // assume deadline is the slack budget
    static const double beta_eff = []{
        const char* e = std::getenv("QCON_REINJECT_BETA");
        double b = 0.1;  // paper default
        if (e) { try { b = std::stod(e); } catch (...) {} }
        return b;
    }();
    bool trigger = current_ect > slack * (1.0 + beta_eff);
    bool alt_ok  = alt_ect <= slack;
    std::string action;
    if (trigger && alt_ok)        action = "reinject";
    else if (trigger && !alt_ok)  action = "no_alt_skip";  // paper rule: don't reinject
    else                          action = "no_trigger";

    if (reinject_logger_) {
        std::ostringstream line;
        line << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() << ","
             << frame_size_kb << "," << deadline_ms << ","
             << cur << "," << cur_bw << "," << current_ect << ","
             << alt << "," << alt_bw << "," << alt_ect << ","
             << slack << "," << (trigger ? "yes" : "no") << ","
             << action;
        reinject_logger_->logLine(line.str());
    }

    // When trigger fires AND alt is viable, send a reinject_burst RC command
    // to pdcp_sender — duplicates next N packets to alt path with HIGH priority
    // queue (paper §5.4 LCID approximation). Burst size defaults to 30 packets;
    // override via QCON_REINJECT_BURST env. Decision itself is NOT mutated, so
    // CCA isn't disturbed by spurious set_path commands; the burst happens
    // alongside the original single-link decision.
    if (action == "reinject" && controller_) {
        // Default 30 (paper §5.4 stable). Tested 5 — empirically WORSE on real
        // traces (-11 to -23%): too little redundancy. Keep paper-canonical 30.
        int burst = 30;
        if (const char* e = std::getenv("QCON_REINJECT_BURST")) {
            try { burst = std::stoi(e); } catch (...) {}
        }
        // QCON_DISABLE_REINJECT=1 fully disables reinject (Fig 16 ablation).
        static const bool reinject_off = []{
            const char* e = std::getenv("QCON_DISABLE_REINJECT");
            return (e && std::string(e) == "1");
        }();
        if (!reinject_off && burst > 0) {
            controller_->sendReinjectBurst(alt, burst);
        }
        return !reinject_off;
    }
    return false;
}

bool QconScheduler::findMinCostMultilinkSolution(
    double frame_size_kb,
    double deadline_ms,
    std::vector<int>& selected_link_ids,
    std::vector<double>& selected_chunk_sizes,
    double& total_cost,
    double& best_completion_time_ms) {  // Added reference parameter

    // Initialize best completion time to max value
    best_completion_time_ms = std::numeric_limits<double>::max();
    
    // Trust raw measured BW: PHY reports per-link trace bandwidth even when the
    // link carries no traffic, so measured=0 means "radio is currently down" and
    // the multilink subset enumerator should not include that link.
    std::vector<std::pair<int, LinkMetrics>> available_links;
    for (const auto& pair : link_metrics_) {
        int link_id = pair.first;
        LinkMetrics m = pair.second;
        if (m.throughput_mbps <= 0.0 &&
            last_known_bw_mbps_.find(link_id) == last_known_bw_mbps_.end()) {
            m.throughput_mbps = 1.0;  // never measured yet → tiny seed
        }
        available_links.push_back({link_id, m});
    }

    // Sort links by cost (assuming link_id is the cost)
    std::sort(available_links.begin(), available_links.end(), 
            [](const auto& a, const auto& b) { return a.first < b.first; });
        
    // Need at least two links for multilink scheduling
    if (available_links.size() < 2) {
        return false;
    }
    
    // Track the best solution found, even if it doesn't meet the deadline
    std::vector<int> best_link_ids;
    std::vector<double> best_chunk_sizes;
    double best_cost = std::numeric_limits<double>::max();
    
    // Start with the lowest cost combinations first
    // Generate all possible combinations of links, starting with 2 links
    for (size_t num_links = 2; num_links <= available_links.size(); num_links++) {
        // Try each combination of 'num_links' links
        std::vector<bool> selector(available_links.size(), false);
        // Set the first 'num_links' elements to true
        std::fill(selector.begin(), selector.begin() + num_links, true);
        
        do {
            // Extract the current link combination
            std::vector<int> link_ids;
            std::vector<double> bandwidths;
            std::vector<double> rtts;
            double current_cost = 0;
            
            for (size_t i = 0; i < available_links.size(); i++) {
                if (selector[i]) {
                    link_ids.push_back(available_links[i].first);
                    bandwidths.push_back(available_links[i].second.throughput_mbps);
                    rtts.push_back(available_links[i].second.delay_ms / 1000.0); // Convert ms to seconds
                    
                    // Add link_id to the cost (assuming link_id represents cost)
                    current_cost += available_links[i].first;
                }
            }
            
            // Compute the optimal chunk distribution
            std::vector<double> chunk_sizes = computeOptimalChunkSizes(
                frame_size_kb, bandwidths, rtts);
            
            // Calculate the max completion time among all links
            double max_completion_time_ms = 0;
            for (size_t i = 0; i < link_ids.size(); i++) {
                if (chunk_sizes[i] > 0) {
                    // Calculate completion time for this chunk
                    double tx_time_ms = (chunk_sizes[i] * 8.0) / bandwidths[i];
                    double rtt_ms = rtts[i] * 1000.0; // Convert seconds back to ms
                    double completion_time_ms = tx_time_ms + rtt_ms;
                    
                    max_completion_time_ms = std::max(max_completion_time_ms, completion_time_ms);
                }
            }
            
            // Update best completion time if this solution is better, regardless of deadline
            if (max_completion_time_ms < best_completion_time_ms) {
                best_completion_time_ms = max_completion_time_ms;
                best_link_ids = link_ids;
                best_chunk_sizes = chunk_sizes;
                best_cost = current_cost;
            }
            
            // Check if this distribution meets the deadline
            if (max_completion_time_ms <= deadline_ms) {
                // Found a valid solution, store it
                selected_link_ids = link_ids;
                selected_chunk_sizes = chunk_sizes;
                total_cost = current_cost;
                
                LOG_MODULE_DEBUG(MODULE, "Found valid multilink solution with " << 
                               num_links << " links, cost: " << current_cost << 
                               ", completion time: " << max_completion_time_ms << 
                               "ms (deadline: " << deadline_ms << "ms)");
                
                // Return the first valid solution (guaranteed to be minimum cost due to ordering)
                return true;
            }
            
        } while (std::prev_permutation(selector.begin(), selector.end()));
    }
    
    // No valid solution found that meets the deadline
    // But we still return the best solution we found in terms of completion time
    if (best_completion_time_ms < std::numeric_limits<double>::max()) {
        selected_link_ids = best_link_ids;
        selected_chunk_sizes = best_chunk_sizes;
        total_cost = best_cost;
        
        LOG_MODULE_DEBUG(MODULE, "No solution meets deadline. Best multilink solution has " <<
                       "completion time: " << best_completion_time_ms << 
                       "ms (deadline: " << deadline_ms << "ms), cost: " << best_cost);
    }
    
    return false;
}

std::vector<double> QconScheduler::computeOptimalChunkSizes(
    double total_size_kb, 
    const std::vector<double>& bandwidths, 
    const std::vector<double>& rtts) {
    
    int N = bandwidths.size();
    std::vector<double> chunk_sizes(N, 0.0);
    
    if (N == 0) {
        return chunk_sizes;
    }
    
    // 1. Compute sum of bandwidths
    double BW_sum = 0.0;
    for (double bw : bandwidths) {
        BW_sum += bw;
    }
    
    if (BW_sum <= 0.0) {
        // Invalid bandwidth sum, return equal chunks
        for (int i = 0; i < N; i++) {
            chunk_sizes[i] = total_size_kb / N;
        }
        return chunk_sizes;
    }
    
    // 2. Compute sum of BW_i * RTT_i
    double BW_RTT_sum = 0.0;
    for (int i = 0; i < N; i++) {
        BW_RTT_sum += bandwidths[i] * rtts[i];
    }
    
    // 3. Compute T (time for all links to finish)
    double T = (total_size_kb * 8.0 / 1000.0 + BW_RTT_sum) / BW_sum; // Convert KB to Mb
    
    // 4. Compute D_i for each link i
    for (int i = 0; i < N; i++) {
        // Di = BW_i * (T - RTT_i)
        double Di = bandwidths[i] * (T - rtts[i]) * 1000.0 / 8.0; // Convert Mb to KB
        
        // If the formula yields a negative number, that implies the link is too slow
        Di = std::max(0.0, Di);
        
        // Apply minimum chunk size if needed
        if (Di > 0 && Di < qcon_config_.min_chunk_size_kb) {
            Di = qcon_config_.min_chunk_size_kb;
        }
        
        chunk_sizes[i] = Di;
    }
    
    // Check if we've allocated too much due to minimum chunk sizes
    double sum_chunks = 0.0;
    for (double chunk : chunk_sizes) {
        sum_chunks += chunk;
    }
    
    if (sum_chunks > total_size_kb && sum_chunks > 0) {
        // Scale down proportionally
        double scale_factor = total_size_kb / sum_chunks;
        for (int i = 0; i < N; i++) {
            chunk_sizes[i] *= scale_factor;
        }
    }
    
    return chunk_sizes;
}

void QconScheduler::onLinkStateUpdated(int link_id, const LinkState& link_state) {
    // Update link metrics
}


void QconScheduler::updateLinkMetrics(const std::map<int, BaseScheduler::LinkMetrics>& metrics) {
    // Convert BaseScheduler::LinkMetrics to QconScheduler::LinkMetrics
    for (const auto& kv : metrics) {
        int link_id = kv.first;
        const BaseScheduler::LinkMetrics& base_metrics = kv.second;
        
        // Create our internal metrics struct
        LinkMetrics our_metrics;
        our_metrics.throughput_mbps = base_metrics.bandwidth_mbps;
        our_metrics.delay_ms = base_metrics.delay_ms;
        our_metrics.queue_size_bytes = base_metrics.queue_size_bytes;
        our_metrics.acked_bytes = base_metrics.acked_bytes;
        
        // Store in our map
        link_metrics_[link_id] = our_metrics;
        
        LOG_MODULE_DEBUG(MODULE, "Link " << link_id << " metrics updated: " 
                   "throughput=" << our_metrics.throughput_mbps << "Mbps, " 
                   "delay=" << our_metrics.delay_ms << "ms, " 
                   "queue_size=" << our_metrics.queue_size_bytes << "bytes, "
                   "acked_bytes=" << our_metrics.acked_bytes << "bytes");

        // Update QoE processor with RLC acked bytes if available
        if (qoe_processor_) {
            qoe_processor_->updateTxMetrics(base_metrics.pdcp_transmitted_bytes, uint64_t(our_metrics.throughput_mbps*1024*1024/(8*1000)), link_id);
        }
    }

    // Trigger a decision update
    //this->runOnce();
}

bool QconScheduler::shouldForwardPackets(int link_id) {
    if (!qcon_config_.enable_packet_forwarding) {
        return false;
    }
    
    auto it = link_metrics_.find(link_id);
    if (it == link_metrics_.end()) {
        return false;
    }
    
    const LinkMetrics& metrics = it->second;
    
    // Check if the link is congested
    if (!metrics.is_congested) {
        return false;
    }
    
    // Check if the delay ratio exceeds the threshold
    double avg_frame_deadline = stats_.avg_frame_deadline > 0 ? stats_.avg_frame_deadline : 33.0;
    double delay_ratio = metrics.delay_ms / avg_frame_deadline;
    
    if (delay_ratio > qcon_config_.delay_threshold_for_forwarding) {
        return true;
    }
    
    return false;
}

double QconScheduler::calculateFrameDeadline(uint32_t frame_timestamp, double remaining_bytes_kb, double last_frame_completion_time_ms) {
    // Convert our link metrics to the format expected by DeadlineSettings
    std::map<int, double> link_frame_bytes;
    std::map<int, double> link_bandwidths;
    std::map<int, double> link_latencies;
    std::map<int, double> link_buffer_bytes;
    
    for (const auto& link_pair : link_metrics_) {
        int link_id = link_pair.first;
        const LinkMetrics& metrics = link_pair.second;
        
        link_frame_bytes[link_id] = remaining_bytes_kb*current_decision_.split_ratio[link_id];
        link_bandwidths[link_id] = metrics.throughput_mbps;
        link_latencies[link_id] = metrics.delay_ms;
        link_buffer_bytes[link_id] = static_cast<double>(metrics.queue_size_bytes);
    }
    
    // Calculate expected delivery time
    double expected_delivery_time = deadline_settings_.calculateExpectedDeliveryTime(
        link_frame_bytes,
        link_bandwidths,
        link_latencies,
        link_buffer_bytes);
    
    // Get target FPS from QoE metrics if available
    double target_fps = 30.0; // Default to 30fps
    if (qoe_processor_) {
        target_fps = qoe_processor_->getQoEMetrics()["avg_frame_rate"].asDouble();
        if (target_fps <= 0) {
            target_fps = 30.0;
        }
    }
    
    // Calculate optimal deadline
    double optimal_deadline = deadline_settings_.calculateOptimalDeadline(
        last_frame_completion_time_ms,
        remaining_bytes_kb,
        target_fps);
    
    LOG_MODULE_DEBUG(MODULE, "Frame " << frame_timestamp << 
                   " deadline calculation: expected_delivery=" << expected_delivery_time << 
                   "ms, optimal_deadline=" << optimal_deadline << "ms");
    
    // Use the greater of the two to ensure we have enough time
    return std::max(expected_delivery_time, optimal_deadline);
}

double QconScheduler::calculateFrameDeadline(uint32_t frame_timestamp, double remaining_bytes_kb, 
                                          double last_frame_completion_time_ms,
                                          std::chrono::steady_clock::time_point first_arrival_time,
                                          double max_jitter_threshold) {
    // First, calculate the standard deadline
    double standard_deadline = calculateFrameDeadline(frame_timestamp, remaining_bytes_kb, last_frame_completion_time_ms);
    
    // Now consider jitter limitations
    auto now = std::chrono::steady_clock::now();
    double elapsed_since_arrival_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - first_arrival_time).count();
    
    // Calculate target arrival-to-completion time based on the previous frame
    double expected_completion_time = elapsed_since_arrival_ms + standard_deadline;
    
    // Calculate how this would affect frame-level jitter
    double est_jitter = std::abs(expected_completion_time - last_frame_completion_time_ms);
    
    // Log the calculations
    LOG_MODULE_DEBUG(MODULE, "Frame " << frame_timestamp << 
                   " jitter-aware deadline calculation: elapsed_since_arrival=" << elapsed_since_arrival_ms << 
                   "ms, standard_deadline=" << standard_deadline << 
                   "ms, estimated_jitter=" << est_jitter << 
                   "ms, max_allowed=" << max_jitter_threshold << "ms");
    
    // If estimated jitter is too high, adjust the deadline
    if (est_jitter > max_jitter_threshold) {
        // Calculate an adjusted deadline that would limit jitter to max_jitter_threshold
        double adjusted_deadline;
        if (expected_completion_time > last_frame_completion_time_ms) {
            // Current estimate is longer than previous, need to reduce
            adjusted_deadline = last_frame_completion_time_ms + max_jitter_threshold - elapsed_since_arrival_ms;
        } else {
            // Current estimate is shorter than previous, need to increase
            adjusted_deadline = last_frame_completion_time_ms - max_jitter_threshold - elapsed_since_arrival_ms;
        }
        
        // Ensure minimum deadline
        adjusted_deadline = std::max(10.0, adjusted_deadline);
        
        LOG_MODULE_INFO(MODULE, "Adjusting frame " << frame_timestamp << 
                     " deadline to limit jitter: " << standard_deadline << 
                     "ms -> " << adjusted_deadline << 
                     "ms (max jitter=" << max_jitter_threshold << "ms)");
        
        return adjusted_deadline;
    }
    
    // If jitter is acceptable, use the standard deadline
    return standard_deadline;
}

Json::Value QconScheduler::getSchedulingDecisionJson(const std::vector<LinkAssignment>& assignments) {
    Json::Value root(Json::arrayValue);
    
    for (const auto& assignment : assignments) {
        Json::Value entry;
        entry["link_id"] = assignment.link_id;
        entry["chunk_size_kb"] = assignment.chunk_size_kb;
        entry["expected_completion_time_ms"] = assignment.expected_completion_time_ms;
        
        root.append(entry);
    }
    
    return root;
}

void QconScheduler::handleNewFrame() {
    LOG_MODULE_DEBUG(MODULE, "New frame notification received");
    
    // Trigger a scheduling decision
    runOnce();
}

} // namespace ric