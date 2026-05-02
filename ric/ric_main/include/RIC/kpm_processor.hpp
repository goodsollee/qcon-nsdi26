#pragma once

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <mutex>
#include <functional>
#include <jsoncpp/json/json.h>

namespace ric {
/**
 * KpmProcessor - Processes Key Performance Metrics
 */
class KpmProcessor {
public:
    KpmProcessor();
    ~KpmProcessor();
    
    /**
     * Initialize the processor with a base log directory
     * 
     * @param base_log_dir Base directory for log files
     * @return true if initialization was successful, false otherwise
     */
    bool initialize(const std::string& base_log_dir,
                              const std::string& scheduler_name,
                              const std::string& config_file_path);
    

    bool processCuMetrics(const std::string& component_id, 
                                  const std::string& metric_type, 
                                  const std::string& metric_value);
    /**
     * Process metrics from a DU component
     * 
     * @param component_id ID of the component that sent the metrics
     * @param metric_type Type of the metric
     * @param metric_value Value of the metric (JSON string)
     * @return true if processing was successful, false otherwise
     */
    bool processDuMetrics(const std::string& component_id, 
                         const std::string& metric_type, 
                         const std::string& metric_value);
    
    bool processRcResponse (const std::string& component_id, uint64_t delay);
    /**
     * Register a callback for a specific metric type
     * 
     * @param metric_type Type of metric to register for
     * @param callback Function to call when this metric is received
     */
    void registerMetricCallback(
        const std::string& metric_type,
        std::function<void(const std::string&, const std::string&, const std::string&)> callback);
    
private:
    std::string base_log_dir_;
    std::string current_log_dir_;
    
    // CSV file for each component
    std::map<std::string, std::ofstream> csv_files_;
    std::map<std::string, bool> headers_written_;
    
    // Mutex for thread safety
    std::mutex file_mutex_;
    std::mutex callback_mutex_;
    
    // Callbacks for different metric types
    std::map<std::string, 
            std::vector<std::function<void(const std::string&, const std::string&, const std::string&)>>> 
        metric_callbacks_;

    // Helper methods
    std::string createTimestampedDirectory(const std::string& base_dir, const std::string& prefix);
    bool parseMetricJson(const std::string& metric_json, Json::Value& root);
    void ensureCsvHeaderExists(const std::string& component_id, const Json::Value& metrics);
    bool writeMetricsToCsv(const std::string& component_id, const Json::Value& metrics);
    
    // Notify callbacks for a metric
    void notifyMetricCallbacks(const std::string& component_id,
                             const std::string& metric_type,
                             const std::string& metric_value);
};

} // namespace ric
