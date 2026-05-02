#include "pdcp_du_link.hpp"
#include "log.hpp"
#include <iostream>

PdcpDULink::PdcpDULink(const PdcpConfig& cfg) 
    : PdcpContext(PdcpRole::LINK), 
      linkId(cfg.du.linkId), 
      packetsForwarded(0),
      ric_interface_(nullptr),
      ric_thread_running_(false) 
{
    
    // Initialize RLC/MAC/PHY modules with segmentation support
    rlcQueueConfigs = cfg.du.queues;
    std::vector<MacSenderModule::QueueHandle> macQueues;
    macQueues.reserve(rlcQueueConfigs.size());

    for (const auto& queueCfg : rlcQueueConfigs) {
        auto rlcModule = std::make_shared<RlcSenderModule>(
            queueCfg.bufferSize ? queueCfg.bufferSize : cfg.du.bufferSize,
            queueCfg.retransmission_timeout_ms ? queueCfg.retransmission_timeout_ms : cfg.du.retransmission_timeout_ms,
            queueCfg.max_retransmissions ? queueCfg.max_retransmissions : cfg.du.max_retransmissions,
            queueCfg.poll_retransmit_timer_ms ? queueCfg.poll_retransmit_timer_ms : cfg.du.poll_retransmit_timer,
            cfg.common.logFoldername);

        rlcModules.push_back(rlcModule);

        MacSenderModule::QueueHandle handle{};
        handle.queueId = queueCfg.queueId;
        handle.priority = queueCfg.priority;
        handle.module = rlcModule;
        macQueues.push_back(handle);

        LOG_INFO("[DU " << linkId << "] Configured RLC queue id="
                 << static_cast<int>(queueCfg.queueId)
                 << " priority=" << static_cast<int>(queueCfg.priority)
                 << " buffer=" << queueCfg.bufferSize);
    }

    if (rlcModules.empty()) {
        // Safety net - should not happen because parser always creates at least one queue
        auto rlcModule = std::make_shared<RlcSenderModule>(
            cfg.du.bufferSize,
            cfg.du.retransmission_timeout_ms,
            cfg.du.max_retransmissions,
            cfg.du.poll_retransmit_timer,
            cfg.common.logFoldername);
        rlcModules.push_back(rlcModule);

        MacSenderModule::QueueHandle handle{};
        handle.queueId = 0;
        handle.priority = 0;
        handle.module = rlcModule;
        macQueues.push_back(handle);
    }

    mac = std::make_shared<MacSenderModule>(macQueues, 1);
    
    // Create PHY module - check if we're using trace file or static bandwidth
    if (!cfg.du.bandwidthTraceFile.empty()) {
        // Dynamic bandwidth mode with trace file
        // For the column selection, we use the linkId value to determine which column to read
        // If linkId is 1, we use column 1 (5g), if 2 we use column 2 (4g), and so on
        int column_index = 1;  // Default to first column (5g)
        
        if (linkId >= 1 && linkId <= 4) {  // Support up to 4 links
            column_index = linkId;  // Use linkId as column index (1-based for user display)
            // Adjust to 0-based internally (column_index-1) since our index is 0-based excluding the time column
        }
        
        // Create PHY module with trace file and column index
        phy = std::make_shared<PhySenderModule>(
            cfg.du.bandwidthTraceFile, 
            cfg.du.bwUpdateInterval ? cfg.du.bwUpdateInterval : 100,
            column_index - 1,  // Adjust to 0-based index excluding time column
            cfg.common.logFoldername
        );
        
        LOG_INFO("[DU " << linkId << "] Using dynamic bandwidth from trace file: " 
                 << cfg.du.bandwidthTraceFile << ", column: " << column_index - 1);
    } else {
        // Static bandwidth mode
        phy = std::make_shared<PhySenderModule>("", cfg.du.bwUpdateInterval ? cfg.du.bwUpdateInterval : 100);
        phy->setBandwidth(cfg.du.fixedBandwidth);
        LOG_INFO("[DU " << linkId << "] Using static bandwidth: " 
                  << (cfg.du.fixedBandwidth / 1000) << " kbps");
    }
    
    // Set up callbacks
    mac->setPhyCallback([this](const unsigned char* packet, size_t len) -> bool {
        if (this->phy) {
            return this->phy->processPacket(packet, len);
        }
        return false;
    });
    
    mac->setGetAvailableBytesCallback([this](double slotDuration) -> size_t {
        if (this->phy) {
            return this->phy->getAvailableBytes(slotDuration);
        }
        return 0;
    });
    
    phy->setDeliveryCallback([this](const unsigned char* packet, size_t len) {
        this->onPhyDelivery(packet, len);
    });
    
    // Start MAC and PHY threads
    mac->start();
    phy->start();

    // Initialize RIC interface if configured
    if (cfg.ric.ric_enabled) {
        try {
            // Create RIC interface wrapper with minimal parameters
            ric_interface_ = std::make_unique<ric::RicInterfaceWrapper>(
                RIC_COMPONENT_DU,  // Component type as string
                "pdcp_du_link" + std::to_string(linkId),  // Match config ID
                cfg.ric.localKpmPort,  // Local KPM port
                cfg.ric.localRcPort,   // Local RC port
                cfg.ric.ipAddress,     // RIC IP
                cfg.ric.kpmPort,       // RIC KPM port
                cfg.ric.rcPort         // RIC RC port
            );
            
            // Initialize and start the interface
            if (!ric_interface_->initInNamespace("node_link"+std::to_string(linkId)+"_s")) {
                LOG_ERROR("Failed to initialize RIC inside node_sender namespace");
            } else {
                LOG_INFO("RIC interface is up and running inside node_link"+std::to_string(linkId)+"_s");

                // Register RC callback using the available API
                if (!ric_interface_->initialize() || 
                    !RIC_ZMQ_SetRcCallback(ric_interface_->getHandle(), handleRicCommand, this)) {
                    LOG_ERROR("[DU] Failed to set RC callback: " << ric_interface_->getLastError());
                } else {
                    LOG_INFO("[DU] RC callback registered successfully");
                }

                // Initialize KPM parameters
                kpm_report_interval_ms_ = cfg.ric.kpm_report_interval_ms ? cfg.ric.kpm_report_interval_ms : 1000;
                last_kpm_report_time_ = std::chrono::steady_clock::now();
                
                // Start the KPM metrics reporting thread
                kpm_thread_running_ = true;
                kpm_metrics_thread_ = std::thread(&PdcpDULink::kpmMetricsThread, this);
                LOG_INFO("[DU] KPM metrics thread started");

                // Start the RIC hello thread
                //ric_thread_running_ = true;
                //ric_hello_thread_ = std::thread(&PdcpDULink::ricHelloThread, this);
                //LOG_INFO("[DU] RIC hello thread started");
                
                // Start the RIC interface
                if (!ric_interface_->start()) {
                    LOG_ERROR("[DU] Failed to start RIC interface: " << ric_interface_->getLastError());
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[DU] Exception in RIC setup: " << e.what());
        }
    }
    
    LOG_INFO("[DU " << linkId << "] Initialized with RLC/MAC/PHY modules and segmentation support");
}

PdcpDULink::~PdcpDULink() {
    // Stop RIC threads
    ric_thread_running_ = false;
    kpm_thread_running_ = false;
    
    // Join threads
    if (ric_hello_thread_.joinable()) {
        ric_hello_thread_.join();
    }
    
    if (kpm_metrics_thread_.joinable()) {
        kpm_metrics_thread_.join();
    }

    // Stop RIC interface
    if (ric_interface_) {
        ric_interface_->stop();
    }

    if (mac) mac->stop();
    if (phy) phy->stop();
    
    LOG_INFO("[DU " << linkId << "] Cleaned up, forwarded " 
              << packetsForwarded.load() << " packets");
}

size_t PdcpDULink::processPacket(unsigned char* packet, size_t len) {
    if (rlcModules.empty()) {
        LOG_ERROR("[DU " << linkId << "] No RLC queues available to enqueue packet");
        return 0;
    }

    // Extract queue ID from PDCP header pad field
    uint8_t queueIndex = 0;
    if (len >= sizeof(pdcp_hdr)) {
        pdcp_hdr* header = reinterpret_cast<pdcp_hdr*>(packet);
        // Use queue ID from PDCP header pad field
        queueIndex = header->pad;

        // Ensure queue index is within valid range
        if (queueIndex >= rlcModules.size()) {
            LOG_WARN("[DU " << linkId << "] Invalid queue ID " << static_cast<int>(queueIndex)
                     << " in packet, using queue 0");
            queueIndex = 0;
        }

        LOG_DEBUG("[DU " << linkId << "] Packet seq=" << ntohl(header->sequence_number)
                  << " with queue_id=" << static_cast<int>(queueIndex)
                  << " routed to queue=" << static_cast<int>(queueIndex)
                  << " (total_queues=" << rlcModules.size() << ")");
    } else {
        LOG_WARN("[DU " << linkId << "] Packet too small for PDCP header, using queue 0");
    }

    // Select target queue based on calculated index
    if (queueIndex >= rlcModules.size()) {
        queueIndex = 0; // Safety fallback
    }

    auto& targetQueue = rlcModules[queueIndex];
    uint8_t queueId = rlcQueueConfigs[queueIndex].queueId;
    uint8_t priority = rlcQueueConfigs[queueIndex].priority;
    bool enqueued = targetQueue->enqueuePacket(packet, len, queueId, priority);

    if (!enqueued) {
        LOG_WARN("[DU " << linkId << "] Warning: Packet dropped due to buffer overflow in queue "
                 << static_cast<int>(queueIndex));
        return 0; // Indicate packet drop
    }

    LOG_INFO("[DU " << linkId << "] Packet enqueued to RLC queue "
             << static_cast<int>(queueIndex)
             << " (priority=" << static_cast<int>(rlcQueueConfigs[queueIndex].priority) << ")");

    // We do not send packet immediately. Instead, MAC layer will pick it up
    return 0;
}

void PdcpDULink::onPhyDelivery(const unsigned char* packet, size_t len) {
    // This is called when PHY layer has successfully "transmitted" the packet
    uint32_t forwardedCount = packetsForwarded.fetch_add(1, std::memory_order_relaxed) + 1;
    
    // Debug output for every 1000 packets
    if (forwardedCount % 1000 == 0) {
        size_t totalBufferPackets = 0;
        size_t totalBufferCapacity = 0;
        size_t totalSegmentPackets = 0;
        for (const auto& module : rlcModules) {
            if (!module) {
                continue;
            }
            totalBufferPackets += module->getBufferOccupancy();
            totalBufferCapacity += module->getBufferCapacity();
            totalSegmentPackets += module->getSegmentBufferOccupancy();
        }

        LOG_INFO("[DU " << linkId << "] Forwarded " << forwardedCount
                  << " packets, current bandwidth: " << (phy->getCurrentBandwidth() / 1000.0)
                  << " kbps, buffer occupancy: " << totalBufferPackets
                  << "/" << totalBufferCapacity
                  << ", segment buffer: " << totalSegmentPackets);
    }
}

uint32_t PdcpDULink::getPacketsForwarded() const {
    return packetsForwarded.load(std::memory_order_relaxed);
}

double PdcpDULink::getThroughput() const {
    if (mac) {
        return mac->getThroughput();
    }
    return 0.0;
}

double PdcpDULink::getLatency() const {
    if (mac) {
        return mac->getAverageLatency();
    }
    return 0.0;
}

uint32_t PdcpDULink::getCurrentBandwidth() const {
    if (phy) {
        return phy->getCurrentBandwidth();
    }
    return 0;
}

size_t PdcpDULink::getRlcBufferOccupancy() const {
    size_t total = 0;
    for (const auto& module : rlcModules) {
        if (module) {
            total += module->getBufferOccupancy();
        }
    }
    return total;
}

void PdcpDULink::setBandwidth(uint32_t bps) {
    if (phy) {
        phy->setBandwidth(bps);
    }
}

void PdcpDULink::setBufferSize(uint32_t size) {
    // Note: This would normally require more complex implementation
    // as changing buffer size dynamically requires careful handling
    LOG_WARN("[DU " << linkId << "] Warning: Dynamic buffer size change not implemented");
}

void PdcpDULink::setSlotDuration(uint32_t ms) {
    // Note: This would normally require restarting the MAC module
    LOG_WARN("[DU " << linkId << "] Warning: Dynamic slot duration change not implemented");
}

void PdcpDULink::setBandwidthTraceFile(const std::string& filename) {
    // Note: This would normally require restarting the PHY module
    LOG_WARN("[DU " << linkId << "] Warning: Dynamic bandwidth trace file change not implemented");
}

void PdcpDULink::ricHelloThread() {
    LOG_INFO("[DU] RIC hello thread started");
    
    // Counter for periodic messages
    int counter = 0;
    
    while (ric_thread_running_) {
        try {
            // Sleep for 1 second
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            if (!ric_thread_running_) {
                break;
            }
            
            uint32_t bufferSize = getRlcBufferOccupancy();
            // Send hello world message every 5 seconds
            // Create a simple JSON message
            struct json_object* hello_obj = json_object_new_object();
            std::string message = "Hello from PDCP DU " + std::to_string(linkId);
            json_object_object_add(hello_obj, "message", json_object_new_string(message.c_str()));
            json_object_object_add(hello_obj, "buffer", json_object_new_int(bufferSize));
            
            const char* hello_str = json_object_to_json_string(hello_obj);
            
            // Send as a KPM metric
            ric_interface_->sendKpmMetric("hello_world", hello_str);
            
            LOG_INFO("[DU] Sent hello world to RIC: " << hello_str);
            
            // Clean up
            json_object_put(hello_obj);
        }
        catch (const std::exception& e) {
            LOG_ERROR("[DU] Exception in RIC hello thread: " << e.what());
            // Continue running after brief pause
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LOG_INFO("[DU] RIC hello thread stopped");
}

void PdcpDULink::kpmMetricsThread() {
    LOG_INFO("[DU] KPM metrics thread started");
    
    while (kpm_thread_running_) {
        try {
            // Sleep for the configured interval
            std::this_thread::sleep_for(std::chrono::milliseconds(kpm_report_interval_ms_));
            
            if (!kpm_thread_running_) {
                break;
            }
            
            
            // Get current timestamp in milliseconds
            auto now = std::chrono::system_clock::now();
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            
            // Get RLC buffer metrics
            uint32_t rlc_buffer_packets = 0;
            uint32_t rlc_segment_buffer_packets = 0;
            size_t rlc_buffer_bytes = 0;
            size_t rlc_segment_buffer_bytes = 0;
            for (const auto& module : rlcModules) {
                if (!module) {
                    continue;
                }
                rlc_buffer_packets += static_cast<uint32_t>(module->getBufferOccupancy());
                rlc_segment_buffer_packets += static_cast<uint32_t>(module->getSegmentBufferOccupancy());
                rlc_buffer_bytes += module->getBufferSizeBytes();
                rlc_segment_buffer_bytes += module->getSegmentBufferSizeBytes();
            }
            size_t total_buffer_bytes = rlc_buffer_bytes + rlc_segment_buffer_bytes;

            // Get RLC acknowledged bytes and other stats
            uint32_t acked_packets = 0;
            uint32_t acked_bytes = 0;
            uint32_t retransmitted = 0;
            uint32_t dropped = 0;
            double e2e_delay = 0.0;
            double queue_delay = 0.0;
            double ack_delay = 0.0;
            double rlc_to_mac_delay = 0.0;
            double mac_to_sender_delay = 0.0;
            int64_t latest_e2e = 0;
            int64_t latest_queue = 0;
            int64_t latest_ack = 0;
            int64_t latest_rlc_to_mac = 0;
            int64_t latest_mac_to_sender = 0;
            
            // Get retransmission stats if available
            if (!rlcModules.empty() && rlcModules.front()) {
                RlcRetransmissionManager* rtx_mgr = rlcModules.front()->getRetransmissionManager();
                if (rtx_mgr) {
                    rtx_mgr->getStats(
                        acked_packets,
                        acked_bytes,
                        retransmitted,
                        dropped,
                        e2e_delay,
                        queue_delay,
                        ack_delay,
                        rlc_to_mac_delay,
                        mac_to_sender_delay,
                        latest_e2e,
                        latest_queue,
                        latest_ack,
                        latest_rlc_to_mac,
                        latest_mac_to_sender
                    );
                }
            }
            
            // Create a JSON object for KPM metrics
            struct json_object* kpm_obj = json_object_new_object();
            
            // Add timestamp
            json_object_object_add(kpm_obj, "timestamp", json_object_new_int64(now_ms));
            
            // Add RLC buffer metrics - both packet counts and byte counts
            json_object_object_add(kpm_obj, "rlc_buffer_packets", json_object_new_int(rlc_buffer_packets));
            json_object_object_add(kpm_obj, "rlc_segment_buffer_packets", json_object_new_int(rlc_segment_buffer_packets));
            json_object_object_add(kpm_obj, "rlc_buffer_bytes", json_object_new_int64((int64_t)rlc_buffer_bytes));
            json_object_object_add(kpm_obj, "rlc_segment_buffer_bytes", json_object_new_int64((int64_t)rlc_segment_buffer_bytes));
            json_object_object_add(kpm_obj, "rlc_total_buffer_bytes", json_object_new_int64((int64_t)total_buffer_bytes));
            
            // Add RLC acknowledgment metrics - cumulative and delta
            json_object_object_add(kpm_obj, "rlc_acked_packets", json_object_new_int(acked_packets));
            json_object_object_add(kpm_obj, "rlc_acked_bytes", json_object_new_int(acked_bytes));
            
            // Add RLC delay metrics (averages)
            json_object_object_add(kpm_obj, "rlc_avg_e2e_delay_ms", json_object_new_double(e2e_delay));
            json_object_object_add(kpm_obj, "rlc_avg_queue_delay_ms", json_object_new_double(queue_delay));
            json_object_object_add(kpm_obj, "rlc_avg_ack_delay_ms", json_object_new_double(ack_delay));
            json_object_object_add(kpm_obj, "rlc_avg_rlc_to_mac_delay_ms", json_object_new_double(rlc_to_mac_delay));
            json_object_object_add(kpm_obj, "rlc_avg_mac_to_sender_delay_ms", json_object_new_double(mac_to_sender_delay));
            
            // Add latest delay metrics
            json_object_object_add(kpm_obj, "rlc_latest_e2e_delay_ms", json_object_new_double(latest_e2e));
            json_object_object_add(kpm_obj, "rlc_latest_queue_delay_ms", json_object_new_double(latest_queue));
            json_object_object_add(kpm_obj, "rlc_latest_ack_delay_ms", json_object_new_double(latest_ack));
            json_object_object_add(kpm_obj, "rlc_latest_rlc_to_mac_delay_ms", json_object_new_double(latest_rlc_to_mac));
            json_object_object_add(kpm_obj, "rlc_latest_mac_to_sender_delay_ms", json_object_new_double(latest_mac_to_sender));
            
            // Add other metrics like retransmission stats
            json_object_object_add(kpm_obj, "rlc_retransmitted_packets", json_object_new_int(retransmitted));
            json_object_object_add(kpm_obj, "rlc_dropped_packets", json_object_new_int(dropped));
            
            // Also add hello message since we commented out the hello thread
            json_object_object_add(kpm_obj, "message", json_object_new_string(("DU" + std::to_string(linkId)).c_str()));
            
            // Add throughput information from PHY layer
            if (phy) {
                uint32_t current_bandwidth_bps = phy->getCurrentBandwidth();
                double throughput_mbps = current_bandwidth_bps / 1000000.0;
                json_object_object_add(kpm_obj, "throughput_mbps", json_object_new_double(throughput_mbps));
            }
            
            // Convert to string and send
            const char* kpm_str = json_object_to_json_string(kpm_obj);
            
            // Send as a KPM metric
            if (!ric_interface_->sendKpmMetric("rlc_performance_metrics", kpm_str)) {
                LOG_ERROR("[DU] Failed to send KPM metrics");
            } else {
                LOG_DEBUG("[DU] Sent KPM metrics to RIC: " << kpm_str);
            }
            
            // Clean up
            json_object_put(kpm_obj);
            
            // Update last report time
            last_kpm_report_time_ = std::chrono::steady_clock::now();
        }
        catch (const std::exception& e) {
            LOG_ERROR("[DU] Exception in KPM metrics thread: " << e.what());
            // Continue running after brief pause
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LOG_INFO("[DU] KPM metrics thread stopped");
}

char* PdcpDULink::handleRicCommand(const char* command_type, const char* command_params, void* user_data) {
    PdcpDULink* self = static_cast<PdcpDULink*>(user_data);
    
    LOG_INFO("[DU] Received RC command: " << command_type << ", params: " << command_params);
    
    bool success = self->processRicCommand(command_type, command_params);
    
    // Create response
    struct json_object* response = json_object_new_object();
    json_object_object_add(response, "status", json_object_new_string(success ? "success" : "error"));
    json_object_object_add(response, "message", json_object_new_string(success ? "Command executed successfully" : "Failed to execute command"));
    
    // Convert to string
    const char* response_str = json_object_to_json_string(response);
    char* result = strdup(response_str);
    
    // Clean up
    json_object_put(response);
    
    return result;
}

bool PdcpDULink::processRicCommand(const char* command_type, const char* command_params) {
    if (strcmp(command_type, "exec_poll") == 0) {
        // Execute poll command to RLC
        if (rlcModules.empty()) {
            LOG_ERROR("[DU] RLC module not available");
            return false;
        }

        for (const auto& module : rlcModules) {
            if (!module) {
                continue;
            }
            std::lock_guard<std::mutex> lock(module->getTimerMutex());
            module->setPollBitPending(true);
        }
        LOG_INFO("[DU] Poll bit set on RLC queues");
        return true;
    }
    else if (strcmp(command_type, "exec_sched") == 0) {
        // Execute schedule command to MAC
        if (mac) {
            // You'll need to add this method to your MacSenderModule class
            mac->triggerImmediateScheduling();
            LOG_INFO("[DU] Triggered MAC scheduling");
            return true;
        } else {
            LOG_ERROR("[DU] MAC module not available");
            return false;
        }
    }
    else if (strcmp(command_type, "start_trace") == 0) {
        // Execute schedule command to PHY (for the experiments)
        if (phy) {
            // You'll need to add this method to your MacSenderModule class
            phy->startTrace();
            LOG_INFO("[DU] Triggered Bandwidth trace");
            return true;
        } else {
            LOG_ERROR("[DU] Triggered Bandwidth trace module not available");
            return false;
        }
    }
    else if (strcmp(command_type, "ideal") == 0) {
        // Execute schedule command to PHY (for the experiments)
        if (phy) {
            // You'll need to add this method to your MacSenderModule class
            phy->idealMode();
            LOG_INFO("[DU] Triggered ideal Bandwidth trace");
            return true;
        } else {
            LOG_ERROR("[DU] Triggered ideal trace module not available");
            return false;
        }
    }
    
    LOG_WARN("[DU] Unknown command type: " << command_type);
    return false;
}