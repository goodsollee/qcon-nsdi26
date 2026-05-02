#include "RIC/zmq_interface.hpp"
#include "RIC/kpm_integration.hpp"
#include "RIC/qoe_processor.hpp"
#include "scheduler/scheduler_interface.hpp"
#include "RIC/controller/cu_scheduler_controller.hpp"
#include "RIC/controller/du_scheduler_controller.hpp"
#include "scheduler/QCON/qcon_scheduler.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <unistd.h> // For geteuid()
#include <fstream>
#include <sstream>
#include <vector>

using namespace std;
using namespace ric;

const string MAIN_MODULE = "MAIN";

volatile sig_atomic_t gSignalStatus = 0;

void signal_handler(int signal) {
    gSignalStatus = signal;
}

// Callback for KPM metrics
void kpm_callback(const string& metric_type, const string& metric_value) {
    LOG_MODULE_INFO(MAIN_MODULE, "KPM metric: " << metric_type << " = " << metric_value);
    
    // Here you can process metrics for your multi-connectivity scheduling algorithm
    // For example, update the cross-layer state manager
}

// CSV parsing helper
vector<string> split(const string& s, char delimiter) {
    vector<string> tokens;
    string token;
    istringstream tokenStream(s);
    while (getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}
// Load configuration from JSON file
bool loadConfigFromJson(const string& config_file, vector<RanComponentInfo>& components, 
                       string& ric_ip, string& log_level, SchedulerConfig& scheduler_config) {
    try {
        // Read the file
        ifstream file(config_file);
        if (!file.is_open()) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to open config file: " << config_file);
            return false;
        }
        
        // Parse JSON
        Json::Value root;
        Json::CharReaderBuilder builder;
        JSONCPP_STRING errs;
        if (!Json::parseFromStream(builder, file, &root, &errs)) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to parse JSON: " << errs);
            return false;
        }
        
        // Read RIC configuration
        if (root.isMember("ric")) {
            const Json::Value& ric = root["ric"];
            if (ric.isMember("ip_address")) {
                ric_ip = ric["ip_address"].asString();
                LOG_MODULE_INFO(MAIN_MODULE, "RIC IP set to: " << ric_ip);
            } else {
                LOG_MODULE_ERROR(MAIN_MODULE, "RIC IP not found in config file");
                return false;
            }
            
            if (ric.isMember("log_level")) {
                log_level = ric["log_level"].asString();
                LOG_MODULE_INFO(MAIN_MODULE, "Log level from config: " << log_level);
            }
        } else {
            LOG_MODULE_ERROR(MAIN_MODULE, "RIC configuration not found in config file");
            return false;
        }
        
        // Read RAN components
        if (root.isMember("components") && root["components"].isArray()) {
            const Json::Value& componentsArray = root["components"];
            for (unsigned int i = 0; i < componentsArray.size(); i++) {
                const Json::Value& comp = componentsArray[i];
                
                // Check if component is enabled (default to true if not specified)
                bool enabled = true;
                if (comp.isMember("enabled")) {
                    enabled = comp["enabled"].asBool();
                }
                
                // Skip disabled components
                if (!enabled) {
                    LOG_MODULE_INFO(MAIN_MODULE, "Skipping disabled component: " << comp["id"].asString());
                    continue;
                }
                
                RanComponentInfo info;
                info.component_type = comp["type"].asString();
                info.component_id = comp["id"].asString();
                info.ip_address = comp["ip_address"].asString();
                info.kpm_port = comp["kpm_port"].asInt();
                info.rc_port = comp["rc_port"].asInt();
                
                components.push_back(info);
                LOG_MODULE_INFO(MAIN_MODULE, "Loaded component: " << info.component_type << " " << info.component_id 
                        << " at " << info.ip_address << " (KPM: " << info.kpm_port << ", RC: " << info.rc_port << ")");
            }
        }
        
        if (components.empty()) {
            LOG_MODULE_ERROR(MAIN_MODULE, "No RAN components found in config file");
            return false;
        }
        
        // Read scheduler configuration
        if (root.isMember("scheduler")) {
            const Json::Value& sched = root["scheduler"];
            
            // Check if scheduler is enabled (default to true if not specified)
            bool scheduler_enabled = true;
            if (sched.isMember("enabled")) {
                scheduler_enabled = sched["enabled"].asBool();
            }
            
            // Store enabled flag in a field added to the configuration
            scheduler_config.is_enabled = scheduler_enabled;
            
            if (!scheduler_enabled) {
                LOG_MODULE_INFO(MAIN_MODULE, "Scheduler is disabled in configuration");
                // Return early but still consider this a successful load
                return true;
            }
            
            if (sched.isMember("scheduler_name")) {
                scheduler_config.scheduler_name = sched["scheduler_name"].asString();
            }
            
            if (sched.isMember("enable_logging")) {
                scheduler_config.enable_logging = sched["enable_logging"].asBool();
            }
            
            if (sched.isMember("dchannel_alpha")) {
                scheduler_config.dchannel_alpha = sched["dchannel_alpha"].asDouble();
            }
            
            LOG_MODULE_INFO(MAIN_MODULE, "Loaded scheduler configuration: " << scheduler_config.scheduler_name);
        } else {
            LOG_MODULE_WARN(MAIN_MODULE, "Scheduler configuration not found, using defaults");
        }
        
        return true;
    }
    catch (const exception& e) {
        LOG_MODULE_ERROR(MAIN_MODULE, "Error loading config: " << e.what());
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Print compile-time log level
    #if LOGLEVEL_NUM == TRACE_LEVEL
        cout << "Compiled with log level: TRACE" << endl;
    #elif LOGLEVEL_NUM == DEBUG_LEVEL
        cout << "Compiled with log level: DEBUG" << endl;
    #elif LOGLEVEL_NUM == INFO_LEVEL
        cout << "Compiled with log level: INFO" << endl;
    #elif LOGLEVEL_NUM == WARN_LEVEL
        cout << "Compiled with log level: WARN" << endl;
    #elif LOGLEVEL_NUM == ERROR_LEVEL
        cout << "Compiled with log level: ERROR" << endl;
    #else
        cout << "Compiled with log level: UNKNOWN" << endl;
    #endif
    
    string config_file = "config.json";
    if (argc > 1) {
        config_file = argv[1];
    }
    
    cout << "Using config file: " << config_file << endl;
    
    // Set up signal handling for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    LOG_MODULE_INFO(MAIN_MODULE, "Starting RIC...");
    
    try {
        // Load configuration
        vector<RanComponentInfo> components;
        string ric_ip;
        string log_level = "INFO"; // Default log level
        
        SchedulerConfig scheduler_config; // Default scheduler config
        
        if (!loadConfigFromJson(config_file, components, ric_ip, log_level, scheduler_config)) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to load configuration");
            return 1;
        }

        // Set log level if specified in config
        if (!log_level.empty()) {
            // Convert string to log level enum and set it
            int level = INFO_LEVEL; // Default
            if (log_level == "TRACE") level = TRACE_LEVEL;
            else if (log_level == "DEBUG") level = DEBUG_LEVEL;
            else if (log_level == "INFO") level = INFO_LEVEL;
            else if (log_level == "WARN") level = WARN_LEVEL;
            else if (log_level == "ERROR") level = ERROR_LEVEL;
            
            LogManager::setGlobalLogLevel(level);
            LOG_MODULE_INFO(MAIN_MODULE, "Set global log level to " << log_level);
        }

        //LogManager::setModuleLogLevel("DCHANNEL_SCHEDULER", INFO_LEVEL);
        //LogManager::setModuleLogLevel("QOE_PROCESSOR", WARN_LEVEL);
        //LogManager::setModuleLogLevel("QCON_SCHEDULER", DEBUG_LEVEL);
        //LogManager::setModuleLogLevel("MINRTT_SCHEDULER", DEBUG_LEVEL);
        //LogManager::setModuleLogLevel("BASE_SCHEDULER", DEBUG_LEVEL);
        
        // Initialize ZMQ interface
        ZmqInterface zmq_interface;
        
        // Initialize RIC with its own IP
        if (!zmq_interface.initialize(ric_ip)) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to initialize RIC");
            return 1;
        }

        auto kpm_processor = make_shared<KpmProcessor>();
        kpm_processor->initialize(
            "logs",
            scheduler_config.scheduler_name,
            config_file
        );

        if (!zmq_interface.registerKpmProcessor(kpm_processor)) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to register KPM processor");
            return 1;
        }

        auto state_manager = make_shared<CrossLayerStateManager>();
        state_manager->initialize();

        KpmStateIntegration kpm_integration(kpm_processor, state_manager);
        kpm_integration.initialize();
        
        
        // Create scheduler with configuration if enabled
        shared_ptr<SchedulerInterface> scheduler;
        shared_ptr<CuSchedulerController> cu_controller;
        shared_ptr<DuSchedulerController> du_controller;
        shared_ptr<QoEProcessor> qoe_processor;

        // Initialize QoE processor if scheduler is QCON
        if (scheduler_config.scheduler_name == "qcon") {
            LOG_MODULE_INFO(MAIN_MODULE, "Initializing QoE processor for QCON scheduler");
            
            qoe_processor = make_shared<QoEProcessor>();
            
            // Try to get effective UID to determine if we have root privileges
            uid_t euid = geteuid();
            bool has_root = (euid == 0);
            
            // Initialize with the TUN interface (use test mode if not root)
            bool test_mode = !has_root;
            
            if (!has_root) {
                LOG_MODULE_WARN(MAIN_MODULE, "Not running as root, QoE processor will use test mode");
            }
            
            if (!qoe_processor->initialize("tun_host", test_mode)) {
                LOG_MODULE_ERROR(MAIN_MODULE, "Failed to initialize QoE processor");
                // Continue anyway, as this might be a testing environment without TUN interface
                LOG_MODULE_WARN(MAIN_MODULE, "Continuing without QoE processor");
            } else {
                // Start QoE processor
                if (!qoe_processor->start()) {
                    LOG_MODULE_ERROR(MAIN_MODULE, "Failed to start QoE processor");
                    // Continue anyway
                    LOG_MODULE_WARN(MAIN_MODULE, "Continuing without QoE processing");
                } else {
                    if (test_mode) {
                        LOG_MODULE_INFO(MAIN_MODULE, "QoE processor started in test mode (simulated data)");
                    } else {
                        LOG_MODULE_INFO(MAIN_MODULE, "QoE processor started with XDP/BPF on tun_host");
                    }
                    
                    // Pass the ZMQ interface pointer to QoE processor
                    auto& zmq_interface_ref = zmq_interface;
                    qoe_processor->setZmqInterface(std::shared_ptr<ZmqInterface>(&zmq_interface_ref, [](ZmqInterface*){}));
                    LOG_MODULE_INFO(MAIN_MODULE, "Set ZMQ interface for QoE processor");

                    // Wire KPM messages into QoE Monitor's frame tracker.
                    // CU pdcp_path_stats → updateTxMetrics(pdcp_total, 0, link)
                    // DU rlc_performance_metrics → updateTxMetrics(0, acked, link)
                    auto qoe_ref = qoe_processor;
                    kpm_processor->registerMetricCallback("pdcp_path_stats",
                        [qoe_ref](const std::string&, const std::string&, const std::string& val) {
                            Json::Reader r; Json::Value root;
                            if (!r.parse(val, root) || !root.isMember("path_bytes") || !root["path_bytes"].isObject()) return;
                            for (const auto& key : root["path_bytes"].getMemberNames()) {
                                auto pos = key.find_last_of('_');
                                if (pos == std::string::npos) continue;
                                int link_id;
                                try { link_id = std::stoi(key.substr(pos+1)); } catch(...) { continue; }
                                uint64_t pdcp_total = root["path_bytes"][key].asUInt64();
                                qoe_ref->updateTxMetrics(pdcp_total, 0, link_id);
                            }
                        });
                    kpm_processor->registerMetricCallback("rlc_performance_metrics",
                        [qoe_ref](const std::string&, const std::string&, const std::string& val) {
                            Json::Reader r; Json::Value root;
                            if (!r.parse(val, root)) return;
                            if (!root.isMember("rlc_acked_bytes") || !root.isMember("link_id")) return;
                            qoe_ref->updateTxMetrics(0, root["rlc_acked_bytes"].asUInt64(),
                                                     root["link_id"].asInt());
                        });
                    LOG_MODULE_INFO(MAIN_MODULE, "Registered KPM→QoE callbacks (pdcp_path_stats + rlc_performance_metrics)");
                }
            }
        }

        // We need to add is_enabled field to SchedulerConfig struct
        if (scheduler_config.is_enabled) {
            cu_controller = make_shared<CuSchedulerController>(zmq_interface);
            scheduler = SchedulerFactory::createScheduler(
                scheduler_config.scheduler_name,
                state_manager,
                scheduler_config,
                cu_controller
            );
            
            if (!scheduler) {
                LOG_MODULE_ERROR(MAIN_MODULE, "Failed to create scheduler: " << scheduler_config.scheduler_name);
                return 1;
            }
            
            // Set QoE processor if this is a QCON scheduler
            if (scheduler_config.scheduler_name == "qcon" && qoe_processor) {
                auto qcon_scheduler = dynamic_pointer_cast<QconScheduler>(scheduler);
                if (qcon_scheduler) {
                    LOG_MODULE_INFO(MAIN_MODULE, "Setting QoE processor for QCON scheduler");
                    try {
                        qcon_scheduler->setQoEProcessor(qoe_processor);
                        LOG_MODULE_INFO(MAIN_MODULE, "Successfully set QoE processor for QCON scheduler");
                    } catch (const std::exception& e) {
                        LOG_MODULE_ERROR(MAIN_MODULE, "Error setting QoE processor: " << e.what());
                    }
                } else {
                    LOG_MODULE_ERROR(MAIN_MODULE, "Failed to cast scheduler to QconScheduler");
                }
            }
            
            // Start the scheduler. Default 100 ms tick so QCON can react to 5G drops
            // mid-trace (paper deadline is 100 ms; 1 s tick was too coarse). Earlier
            // attempt at 100 ms alone was bad because frame_size_kb=1 made decisions
            // trivial; with realistic 50 KB default frame, fast tick is helpful.
            // Override with env var QCON_SCHED_INTERVAL_MS for ablation.
            int sched_interval_ms = 100;
            if (const char* env = std::getenv("QCON_SCHED_INTERVAL_MS")) {
                try { sched_interval_ms = std::stoi(env); } catch(...) {}
            }
            scheduler_config.min_switch_interval_ms = std::min(scheduler_config.min_switch_interval_ms, sched_interval_ms);
            if (!scheduler->start(sched_interval_ms)) {
                LOG_MODULE_ERROR(MAIN_MODULE, "Failed to start scheduler");
                return 1;
            }
            
            LOG_MODULE_INFO(MAIN_MODULE, "Started scheduler: " << scheduler_config.scheduler_name);
        } else {
            LOG_MODULE_INFO(MAIN_MODULE, "Scheduler is disabled in configuration, running without scheduler");
        }
        
        // Register all RAN components
        for (const auto& component : components) {
            if (!zmq_interface.registerRanComponent(component)) {
                LOG_MODULE_ERROR(MAIN_MODULE, "Failed to register component: " << component.component_id);
                return 1;
            }
        }
        
        // Connect to all registered components
        if (!zmq_interface.connectToRanComponents()) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to connect to RAN components");
            return 1;
        }
        
        // Set callbacks for KPM metrics
        for (const auto& component : components) {
            zmq_interface.setKpmCallback(component.component_id, 
                [&component](const string& metric_type, const string& metric_value) {
                    // TODO
                }
            );
        }
        
        // Start listening for KPM metrics
        if (!zmq_interface.startListening()) {
            LOG_MODULE_ERROR(MAIN_MODULE, "Failed to start listening");
            return 1;
        }
        
        LOG_MODULE_INFO(MAIN_MODULE, "RIC is running. Press Ctrl+C to exit.");
        
        // Main loop - sends bootstrap commands once, then idles.
        // Configurable startup delay (default 6s) lets WebRTC ICE handshake +
        // CCA initial ramp complete BEFORE trace replay starts. Without this,
        // trace t=0 (typically 5G=0 in cellular traces) coincides with WebRTC
        // startup so 18%+ of slots show "missed opportunity" but really WebRTC
        // simply wasn't sending yet. Override via QCON_STARTUP_DELAY_MS env.
        int startup_delay_ms = 6000;
        if (const char* env = std::getenv("QCON_STARTUP_DELAY_MS")) {
            try { startup_delay_ms = std::stoi(env); } catch (...) {}
        }
        if (startup_delay_ms > 0) {
            LOG_MODULE_INFO(MAIN_MODULE, "Deferring trace bootstrap by "
                            << startup_delay_ms << " ms (waiting for WebRTC to start)");
            this_thread::sleep_for(chrono::milliseconds(startup_delay_ms));
        }
        while (gSignalStatus == 0) {
            // Send a one-shot bootstrap command on the first iteration only.
            static int counter = 0;
            if (counter == 0 && !components.empty()) {
                std::vector<std::string> bootstrap_cmds;
                if (scheduler_config.scheduler_name == "ideal") {
                    bootstrap_cmds.push_back("ideal");
                }
                bootstrap_cmds.push_back("start_trace");
                for (const auto& cmd : bootstrap_cmds) {
                    for (const auto& component : components) {
                        LOG_MODULE_INFO(MAIN_MODULE, "Bootstrap command '" << cmd
                                        << "' to " << component.component_id);
                        zmq_interface.sendRcCommand(component.component_id,
                                                    cmd,
                                                    "param1=value1,param2=value2");
                    }
                }
            }
            ++counter;
            this_thread::sleep_for(chrono::seconds(5));
        }
        
        // Clean shutdown
        LOG_MODULE_INFO(MAIN_MODULE, "Shutting down RIC...");
        zmq_interface.stopListening();
        
        return 0;
    }
    catch (const exception& e) {
        LOG_MODULE_ERROR(MAIN_MODULE, "Exception: " << e.what());
        return 1;
    }
}