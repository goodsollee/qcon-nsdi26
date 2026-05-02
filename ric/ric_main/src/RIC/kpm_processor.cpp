// src/RIC/kpm_processor.cpp
#include "RIC/kpm_processor.hpp"
#include "log.hpp"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>

using namespace std;
const string KPM_MODULE = "KPM_PROCESSOR";
namespace fs = std::filesystem;
namespace ric {

KpmProcessor::KpmProcessor() {
    LOG_MODULE_DEBUG(KPM_MODULE,"KpmProcessor constructor");
}

KpmProcessor::~KpmProcessor() {
    LOG_MODULE_DEBUG(KPM_MODULE,"KpmProcessor destructor");
    
    // Close all open CSV files
    std::lock_guard<std::mutex> lock(file_mutex_);
    for (auto& [component_id, file] : csv_files_) {
        if (file.is_open()) {
            file.close();
        }
    }
}

bool KpmProcessor::initialize(const std::string& base_log_dir,
                              const std::string& scheduler_name,
                              const std::string& config_file_path)
{
    lock_guard<mutex> lock(file_mutex_);
    base_log_dir_ = base_log_dir;

    try {
        // 1) ensure base exists
        if (!fs::exists(base_log_dir_)) {
            fs::create_directories(base_log_dir_);
            LOG_MODULE_INFO(KPM_MODULE, "Created base log dir: " << base_log_dir_);
        }

        // 2) make scheduler-prefixed, timestamped subdir
        current_log_dir_ = createTimestampedDirectory(base_log_dir_, scheduler_name);
        LOG_MODULE_INFO(KPM_MODULE, "Logs will go into: " << current_log_dir_);

        // 3) copy the JSON config into that folder (if provided)
        if (!config_file_path.empty() && fs::exists(config_file_path)) {
            auto dest = current_log_dir_ + "/config.json";
            fs::copy_file(config_file_path,
                          dest,
                          fs::copy_options::overwrite_existing);
            LOG_MODULE_INFO(KPM_MODULE, "Copied config to: " << dest);
        }

        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "initialize failed: " << e.what());
        return false;
    }
}

void KpmProcessor::registerMetricCallback(
    const std::string& metric_type,
    std::function<void(const std::string&, const std::string&, const std::string&)> callback
) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    metric_callbacks_[metric_type].push_back(callback);
    LOG_MODULE_INFO(KPM_MODULE, "Registered callback for metric type: " << metric_type);
}

void KpmProcessor::notifyMetricCallbacks(
    const std::string& component_id,
    const std::string& metric_type,
    const std::string& metric_value
) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    
    auto it = metric_callbacks_.find(metric_type);
    if (it != metric_callbacks_.end()) {
        for (const auto& callback : it->second) {
            try {
                callback(component_id, metric_type, metric_value);
            } catch (const std::exception& e) {
                LOG_MODULE_ERROR(KPM_MODULE, "Exception in metric callback: " << e.what());
            }
        }
    }
}

bool KpmProcessor::processCuMetrics(const std::string& component_id, 
                                  const std::string& metric_type, 
                                  const std::string& metric_value) {
    // Only process metrics we're interested in
    if (metric_type != "pdcp_path_stats") {
        LOG_MODULE_DEBUG(KPM_MODULE, "Ignoring non-PDCP path stats metric: " << metric_type);
        return true;
    }
    
    LOG_MODULE_DEBUG(KPM_MODULE, "Processing CU metric from " << component_id << ": " << metric_type);
    
    try {
        Json::Value root;
        if (!parseMetricJson(metric_value, root)) {
            LOG_MODULE_ERROR(KPM_MODULE, "Failed to parse metric JSON from " << component_id);
            return false;
        }

        // Notify callbacks about this metric
        notifyMetricCallbacks(component_id, metric_type, metric_value);
        
        // Check if this is a CU metric by verifying it has path_bytes and active_path fields
        if (root.isMember("message") && root["message"].asString().find("CU") != std::string::npos) {
            // This is a CU metric, process it
            LOG_MODULE_DEBUG(KPM_MODULE, "Found CU path metric from " << component_id 
                          << " with active path: " << root["active_path"].asInt());
            
            // Flatten the path_bytes object into individual columns
            if (root.isMember("path_bytes") && root["path_bytes"].isObject()) {
                Json::Value pathBytes = root["path_bytes"];
                
                // Remove the original path_bytes object
                root.removeMember("path_bytes");
                
                // Add each path as its own column
                for (const auto& pathName : pathBytes.getMemberNames()) {
                    // Copy the path value directly to the root object
                    // This will create columns like "path_0", "path_1", etc.
                    root[pathName] = pathBytes[pathName];
                }
            }
            
            // Write metrics to CSV with flattened path values
            return writeMetricsToCsv(component_id, root);
        }
        else {
            LOG_MODULE_DEBUG(KPM_MODULE, "Metric does not contain expected CU fields, skipping");
            return true;
        }
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "Error processing CU metrics from " << component_id << ": " << e.what());
        return false;
    }
}

// Modify the processDuMetrics method to include callback notification:

bool KpmProcessor::processDuMetrics(const std::string& component_id, 
                                  const std::string& metric_type, 
                                  const std::string& metric_value) {
    // Only process metrics we're interested in
    if (metric_type != "rlc_performance_metrics") {
        LOG_MODULE_DEBUG(KPM_MODULE, "Ignoring non-RLC performance metric: " << metric_type);
        return true;
    }
    
    LOG_MODULE_DEBUG(KPM_MODULE, "Processing DU metric from " << component_id << ": " << metric_type);
    
    try {
        Json::Value root;
        if (!parseMetricJson(metric_value, root)) {
            LOG_MODULE_ERROR(KPM_MODULE, "Failed to parse metric JSON from " << component_id);
            return false;
        }
        
        // Notify callbacks about this metric
        notifyMetricCallbacks(component_id, metric_type, metric_value);
        
        // Check if this is a DU metric by looking for the "message" field
        if (root.isMember("message") && root["message"].asString().find("DU") != std::string::npos) {
            // This is a DU metric, process it
            LOG_MODULE_DEBUG(KPM_MODULE, "Found DU metric from " << component_id << ": " << root["message"].asString());
            
            // Write metrics to CSV
            return writeMetricsToCsv(component_id, root);
        }
        else {
            LOG_MODULE_DEBUG(KPM_MODULE, "Metric does not contain DU identifier, skipping");
            return true;
        }
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "Error processing DU metrics from " << component_id << ": " << e.what());
        return false;
    }
}

bool KpmProcessor::processRcResponse(const std::string& component_id, uint64_t delay) {
    LOG_MODULE_DEBUG(KPM_MODULE, "Processing RC delay metric from " << component_id << ": " << delay);
    
    try {
        // Create a simplified JSON object with just timestamp and delay
        Json::Value delayMetric;
        
        // Get current time for timestamp
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_tm = *std::localtime(&now_time_t);
        
        // Format timestamp: YYYY-MM-DD HH:MM:SS
        std::stringstream timestamp;
        timestamp << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
        
        // Set properties in JSON
        delayMetric["timestamp"] = timestamp.str();
        delayMetric["delay"] = Json::Value::UInt64(delay);
        
        // Ensure we have a dedicated file for RC delay metrics
        std::lock_guard<std::mutex> lock(file_mutex_);
        std::string file_key = component_id + "_rc_delay";
        
        // Check if we need to create a header for this file
        if (!headers_written_[file_key]) {
            std::string filename = current_log_dir_ + "/" + component_id + "_rc_delay.csv";
            csv_files_[file_key].open(filename, std::ios::out | std::ios::app);
            
            if (!csv_files_[file_key].is_open()) {
                LOG_MODULE_ERROR(KPM_MODULE, "Failed to open RC delay CSV file for " << component_id);
                return false;
            }
            
            LOG_MODULE_INFO(KPM_MODULE, "Opened RC delay CSV file for " << component_id << ": " << filename);
            
            // Create header
            csv_files_[file_key] << "timestamp,delay\n";
            csv_files_[file_key].flush();
            
            headers_written_[file_key] = true;
        }
        
        // Write data to CSV
        csv_files_[file_key] << delayMetric["timestamp"].asString() << "," << delay << "\n";
        csv_files_[file_key].flush();
        
        LOG_MODULE_DEBUG(KPM_MODULE, "Wrote RC delay to CSV for " << component_id);
        
        // Notify any callbacks about this metric
        // Convert the delay to string for the callback
        std::string delay_str = std::to_string(delay);
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "Error processing RC delay for " << component_id << ": " << e.what());
        return false;
    }
}

std::string KpmProcessor::createTimestampedDirectory(const std::string& base_dir,
                                                     const std::string& prefix)
{
    // timestamp string
    auto now        = std::chrono::system_clock::now();
    auto now_t      = std::chrono::system_clock::to_time_t(now);
    std::stringstream ts;
    ts << std::put_time(std::localtime(&now_t), "%Y-%m-%d_%H-%M-%S");

    // <base>/<prefix>_<timestamp>
    std::string dir = base_dir + "/" + prefix + "_" + ts.str();
    fs::create_directories(dir);
    LOG_MODULE_INFO(KPM_MODULE, "Created log directory: " << dir);
    return dir;
}

bool KpmProcessor::parseMetricJson(const std::string& metric_json, Json::Value& root) {
    Json::Reader reader;
    bool success = reader.parse(metric_json, root);
    
    if (!success) {
        LOG_MODULE_ERROR(KPM_MODULE, "Failed to parse metric JSON: " << reader.getFormattedErrorMessages());
        return false;
    }
    
    return true;
}

void KpmProcessor::ensureCsvHeaderExists(const std::string& component_id, const Json::Value& metrics) {
    // Make a local copy of component_id for safety
    std::string local_component_id = component_id;
    
    LOG_MODULE_DEBUG(KPM_MODULE, "ensureCsvHeaderExists called for " << local_component_id);
    
    try {
        std::lock_guard<std::mutex> lock(file_mutex_);
        LOG_MODULE_DEBUG(KPM_MODULE, "Lock acquired for " << local_component_id);
        
        // Safely check if headers are already written
        auto header_it = headers_written_.find(local_component_id);
        if (header_it != headers_written_.end() && header_it->second) {
            LOG_MODULE_DEBUG(KPM_MODULE, "Headers already written for " << local_component_id << ", returning");
            return;
        }
        
        LOG_MODULE_DEBUG(KPM_MODULE, "Headers not written yet for " << local_component_id);
        
        // Safely check if file is already open
        auto file_it = csv_files_.find(local_component_id);
        bool file_is_open = (file_it != csv_files_.end() && file_it->second.is_open());
        
        if (!file_is_open) {
            std::string filename = current_log_dir_ + "/" + local_component_id + "_metrics.csv";
            LOG_MODULE_DEBUG(KPM_MODULE, "Opening file: " << filename << " for " << local_component_id);
            
            // Create a temporary ofstream
            std::ofstream temp_file(filename, std::ios::out | std::ios::app);
            
            if (!temp_file.is_open()) {
                throw std::runtime_error("Failed to open CSV file for " + local_component_id);
            }
            
            // Only after successful open, insert into the map using insert_or_assign
            csv_files_.insert_or_assign(local_component_id, std::move(temp_file));
            
            LOG_MODULE_INFO(KPM_MODULE, "Opened CSV file for " << local_component_id << ": " << filename);
        } else {
            LOG_MODULE_DEBUG(KPM_MODULE, "File already open for " << local_component_id);
        }
        
        // Get a reference to the file after ensuring it exists in the map
        auto& csv_file = csv_files_.at(local_component_id);
        
        // Check if file is empty (needs header)
        LOG_MODULE_DEBUG(KPM_MODULE, "Checking if file is empty for " << local_component_id);
        csv_file.seekp(0, std::ios::end);
        
        if (csv_file.tellp() == 0) {
            LOG_MODULE_DEBUG(KPM_MODULE, "File is empty, writing header for " << local_component_id);
            
            // Write header based on the keys in the metrics JSON
            std::stringstream header;
            header << "timestamp";
            
            // Get member names safely
            std::vector<std::string> member_names = metrics.getMemberNames();
            
            // Add all keys from the metrics JSON as columns
            for (const auto& key : member_names) {
                if (key != "timestamp" && key != "message") {
                    header << "," << key;
                }
            }
            header << "\n";
            
            // Get the header string before writing to file
            std::string header_str = header.str();
            
            LOG_MODULE_DEBUG(KPM_MODULE, "Header string built: " << header_str);
            csv_file << header_str;
            csv_file.flush();
            
            LOG_MODULE_INFO(KPM_MODULE, "Created CSV header for " << local_component_id);
        } else {
            LOG_MODULE_DEBUG(KPM_MODULE, "File not empty, header already exists");
        }
        
        LOG_MODULE_DEBUG(KPM_MODULE, "Setting headers_written_[" << local_component_id << "] = true");
        headers_written_[local_component_id] = true;
        LOG_MODULE_DEBUG(KPM_MODULE, "ensureCsvHeaderExists completed for " << local_component_id);
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "Exception in ensureCsvHeaderExists for " << local_component_id 
                        << ": " << e.what());
    } catch (...) {
        LOG_MODULE_ERROR(KPM_MODULE, "Unknown exception in ensureCsvHeaderExists for " << local_component_id);
    }
}

bool KpmProcessor::writeMetricsToCsv(const std::string& component_id, const Json::Value& metrics) {
    try {
        // Ensure CSV header exists
        ensureCsvHeaderExists(component_id, metrics);
        
        std::lock_guard<std::mutex> lock(file_mutex_);
        
        // Prepare the CSV line
        std::stringstream line;
        
        // Add timestamp
        if (metrics.isMember("timestamp")) {
            line << metrics["timestamp"].asString();
        }
        else {
            // Use current time if no timestamp in metrics
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            line << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");
        }
        
        // Add all other values in the same order as the header
        for (const auto& key : metrics.getMemberNames()) {
            if (key != "timestamp" && key != "message") {
                line << ",";
                
                // Handle different JSON value types
                if (metrics[key].isString()) {
                    line << metrics[key].asString();
                }
                else if (metrics[key].isInt()) {
                    line << metrics[key].asInt();
                }
                else if (metrics[key].isDouble()) {
                    line << metrics[key].asDouble();
                }
                else if (metrics[key].isBool()) {
                    line << (metrics[key].asBool() ? "true" : "false");
                }
                else if (metrics[key].isObject()) {
                    // For nested objects like path_bytes, convert to JSON string
                    Json::FastWriter writer;
                    std::string jsonStr = writer.write(metrics[key]);
                    // Remove newlines from the JSON string
                    jsonStr.erase(std::remove(jsonStr.begin(), jsonStr.end(), '\n'), jsonStr.end());
                    line << "\"" << jsonStr << "\"";
                }
                else if (metrics[key].isArray()) {
                    // For arrays, also convert to JSON string
                    Json::FastWriter writer;
                    std::string jsonStr = writer.write(metrics[key]);
                    jsonStr.erase(std::remove(jsonStr.begin(), jsonStr.end(), '\n'), jsonStr.end());
                    line << "\"" << jsonStr << "\"";
                }
                else {
                    // Last resort - try string conversion or use empty string
                    try {
                        line << metrics[key].asString();
                    } catch (...) {
                        LOG_MODULE_WARN(KPM_MODULE, "Could not convert field '" << key << "' to string, using empty value");
                        line << "";
                    }
                }
            }
        }
        line << "\n";
        
        // Write to CSV file
        csv_files_[component_id] << line.str();
        csv_files_[component_id].flush();
        
        LOG_MODULE_DEBUG(KPM_MODULE, "Wrote metrics to CSV for " << component_id);
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(KPM_MODULE, "Error writing metrics to CSV for " << component_id << ": " << e.what());
        return false;
    }
}

} // namespace ric