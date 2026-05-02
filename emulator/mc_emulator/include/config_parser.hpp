#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include "pdcp_config.h"

// Include full JsonCpp header
#include <jsoncpp/json/json.h>

/**
 * Class for parsing emulator configuration from JSON
 */
class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    std::vector<PdcpConfig> getAllPdcpConfigs() const;

    /**
     * Load configuration from a JSON file
     * 
     * @param filename Path to the JSON configuration file
     * @return true if loading was successful, false otherwise
     */
    bool loadConfig(const std::string& filename);

    /**
     * Get the PDCP configuration for a specific link
     * 
     * @param linkId ID of the link to get configuration for
     * @return PDCP configuration for the specified link
     */
    PdcpConfig getPdcpConfig(int linkId) const;

    /**
     * Get the trace column index for a specific link
     * 
     * @param linkId ID of the link to get trace column for
     * @return Column index in the trace file (0-based)
     */
    int getTraceColumn(int linkId) const;

    /**
     * Get the number of links configured
     * 
     * @return Number of links
     */
    int getNumLinks() const;

    /**
     * Get the bandwidth trace file path
     * 
     * @return Path to the bandwidth trace file
     */
    std::string getTraceFilePath() const;

    /**
     * Get the configured log level
     * 
     * @return The log level as a string
     */
    std::string getLogLevel() const;

private:
    Json::Value root;
    
    // Helper methods to extract values from JSON
    std::string getString(const Json::Value& obj, const std::string& key, const std::string& defaultValue) const;
    int getInt(const Json::Value& obj, const std::string& key, int defaultValue) const;
    bool getBool(const Json::Value& obj, const std::string& key, bool defaultValue) const;
};

#endif // CONFIG_PARSER_HPP