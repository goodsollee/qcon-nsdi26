#include "config_parser.hpp"
#include "log.hpp"

#define MODULE_PARSER "ConfigParser"

ConfigParser::ConfigParser() {
    // Initialize with empty JSON object
    root = Json::Value(Json::objectValue);
}

ConfigParser::~ConfigParser() {
    // No need for explicit cleanup
}

std::vector<PdcpConfig> ConfigParser::getAllPdcpConfigs() const {
    std::vector<PdcpConfig> allConfigs;
    
    // First get the number of links
    int numLinks = getNumLinks();
    LOG_MODULE_INFO(MODULE_PARSER,"Retrieving configurations for " << numLinks << " links");
    
    // Get CU configuration (link ID 0)
    PdcpConfig cuConfig = getPdcpConfig(0);
    allConfigs.push_back(cuConfig);
    
    // Get configuration for each DU link
    for (int linkId = 1; linkId <= numLinks; linkId++) {
        PdcpConfig linkConfig = getPdcpConfig(linkId);
        allConfigs.push_back(linkConfig);
        LOG_MODULE_DEBUG(MODULE_PARSER,"Retrieved configuration for link " << linkId);
    }
    
    LOG_MODULE_INFO(MODULE_PARSER,"Successfully retrieved all " << allConfigs.size() << " configurations");
    return allConfigs;
}

bool ConfigParser::loadConfig(const std::string& filename) {
    // Open the file
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOG_MODULE_ERROR(MODULE_PARSER,"Failed to open configuration file: " << filename);
        return false;
    }

    // Parse JSON using modern API
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::String errs;
    bool success = Json::parseFromStream(builder, file, &root, &errs);
    
    if (!success) {
        LOG_MODULE_ERROR(MODULE_PARSER,"Failed to parse JSON configuration from " << filename << ": " << errs);
        return false;
    }

    // Check for and set log level immediately
    if (root.isMember("log_level") && root["log_level"].isString()) {
        std::string logLevel = root["log_level"].asString();
        int numericLevel = LogManager::logLevelFromString(logLevel);
        LogManager::setGlobalLogLevel(numericLevel);
        LOG_MODULE_INFO(MODULE_PARSER,"Setting log level to " << logLevel);
    }

    // Log successful loading
    LOG_MODULE_INFO(MODULE_PARSER,"Successfully loaded configuration from " << filename);
    return true;
}

std::string ConfigParser::getString(const Json::Value& obj, const std::string& key, const std::string& defaultValue) const {
    if (obj.isMember(key) && obj[key].isString()) {
        return obj[key].asString();
    }
    return defaultValue;
}

int ConfigParser::getInt(const Json::Value& obj, const std::string& key, int defaultValue) const {
    if (obj.isMember(key) && obj[key].isInt()) {
        return obj[key].asInt();
    }
    return defaultValue;
}

bool ConfigParser::getBool(const Json::Value& obj, const std::string& key, bool defaultValue) const {
    if (obj.isMember(key) && obj[key].isBool()) {
        return obj[key].asBool();
    }
    return defaultValue;
}

PdcpConfig ConfigParser::getPdcpConfig(int linkId) const {
    PdcpConfig config;
    
    LOG_MODULE_DEBUG(MODULE_PARSER,"Begin parsing configuration for linkId: " << linkId);

    if (root.isMember("num_links") && root["num_links"].isInt()) {
        config.common.path_num = root["num_links"].asInt();
        LOG_MODULE_DEBUG(MODULE_PARSER,"Number of links: " << config.common.path_num);
    } else {
        config.common.path_num = 2; // Default to 2 links
        LOG_MODULE_DEBUG(MODULE_PARSER,"No 'num_links' section found, using default: " << config.common.path_num);
    }

    // Set common configuration
    if (root.isMember("pdcp") && root["pdcp"].isObject()) {
        config.common.logFoldername = getString(root["pdcp"], "log_folder", "emulator_logs");
        LOG_MODULE_DEBUG(MODULE_PARSER,"PDCP log folder: " << config.common.logFoldername);
    } else {
        LOG_MODULE_DEBUG(MODULE_PARSER,"No 'pdcp' section found, using defaults");
    }
    // Set link configuration
    config.du.linkId = linkId;
    LOG_MODULE_DEBUG(MODULE_PARSER,"Setting linkId: " << linkId);
    
    if (root.isMember("rlc") && root["rlc"].isObject()) {
        config.du.bufferSize = getInt(root["rlc"], "buffer_size", 1000);
        config.ue.rlc.bufferSize = getInt(root["rlc"], "buffer_size", 1000);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC buffer size: " << config.du.bufferSize);

        config.du.retransmission_timeout_ms = getInt(root["rlc"], "retransmission_timeout_ms", 300);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC retransmission timeout: " << config.du.retransmission_timeout_ms << " ms");

        config.du.poll_retransmit_timer = getInt(root["rlc"], "poll_retransmit_timer_ms", 1000);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC poll retransmit timer: " << config.du.poll_retransmit_timer << " ms");

        config.du.max_retransmissions = getInt(root["rlc"], "max_retransmissions", 3);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC max retransmissions: " << config.du.max_retransmissions);

        config.ue.rlc.reassembly_timeout_ms = getInt(root["rlc"], "reassembly_timeout_ms", 100);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC reassembly timeout: " << config.ue.rlc.reassembly_timeout_ms << " ms");

        config.ue.rlc.status_pdu_interval_ms = getInt(root["rlc"], "status_pdu_interval_ms", 5);
        LOG_MODULE_DEBUG(MODULE_PARSER,"RLC status PDU interval: " << config.ue.rlc.status_pdu_interval_ms << " ms");

        config.ue.rlc.t_statusProhibit_ms = getInt(root["rlc"], "status_prohibit_timer_ms", 100);
        LOG_MODULE_DEBUG(MODULE_PARSER,"Using 'status_prohibit_timer_ms': " << config.ue.rlc.t_statusProhibit_ms << " ms");

        if (root["rlc"].isMember("queues") && root["rlc"]["queues"].isArray()) {
            const Json::Value& queues = root["rlc"]["queues"];
            LOG_MODULE_DEBUG(MODULE_PARSER,"Parsing " << queues.size() << " RLC queue definitions");

            for (Json::ArrayIndex i = 0; i < queues.size(); ++i) {
                if (!queues[i].isObject()) {
                    LOG_MODULE_WARN(MODULE_PARSER,"Skipping non-object entry at rlc.queues[" << i << "]");
                    continue;
                }

                RlcQueueConfig queueCfg;
                queueCfg.queueId = static_cast<uint8_t>(getInt(queues[i], "id", static_cast<int>(i)));
                queueCfg.priority = static_cast<uint8_t>(getInt(queues[i], "priority", queueCfg.queueId));
                queueCfg.bufferSize = static_cast<uint32_t>(getInt(queues[i], "buffer_size", static_cast<int>(config.du.bufferSize)));
                queueCfg.max_retransmissions = static_cast<uint32_t>(getInt(queues[i], "max_retransmissions", static_cast<int>(config.du.max_retransmissions)));
                queueCfg.retransmission_timeout_ms = static_cast<uint32_t>(getInt(queues[i], "retransmission_timeout_ms", static_cast<int>(config.du.retransmission_timeout_ms)));
                queueCfg.poll_retransmit_timer_ms = static_cast<uint32_t>(getInt(queues[i], "poll_retransmit_timer_ms", static_cast<int>(config.du.poll_retransmit_timer)));

                config.du.queues.push_back(queueCfg);
                config.ue.queues.push_back(queueCfg);

                LOG_MODULE_DEBUG(MODULE_PARSER,
                                 "Added RLC queue id=" << static_cast<int>(queueCfg.queueId)
                                                      << ", priority=" << static_cast<int>(queueCfg.priority)
                                                      << ", buffer=" << queueCfg.bufferSize);
            }
        }
    } else {
        LOG_MODULE_DEBUG(MODULE_PARSER,"No 'rlc' section found, using defaults");
    }

    if (config.du.queues.empty()) {
        RlcQueueConfig queueCfg;
        queueCfg.queueId = 0;
        queueCfg.priority = 0;
        queueCfg.bufferSize = config.du.bufferSize;
        queueCfg.max_retransmissions = config.du.max_retransmissions;
        queueCfg.retransmission_timeout_ms = config.du.retransmission_timeout_ms;
        queueCfg.poll_retransmit_timer_ms = config.du.poll_retransmit_timer;

        config.du.queues.push_back(queueCfg);
        config.ue.queues.push_back(queueCfg);

        LOG_MODULE_DEBUG(MODULE_PARSER,"No explicit RLC queues configured; using single default queue");
    }

    // Set MAC configuration
    if (root.isMember("mac") && root["mac"].isObject()) {
        config.ue.mac.txOpportunityInterval = getInt(root["mac"], "tx_opportunity_interval", 10);
        LOG_MODULE_DEBUG(MODULE_PARSER,"MAC tx opportunity interval: " << config.ue.mac.txOpportunityInterval);
        
        config.ue.mac.txDelay = getInt(root["mac"], "tx_delay", 5);
        LOG_MODULE_DEBUG(MODULE_PARSER,"MAC tx delay: " << config.ue.mac.txDelay);
    } else {
        LOG_MODULE_DEBUG(MODULE_PARSER,"No 'mac' section found, using defaults");
    }

    // Set bandwidth configuration
    if (root.isMember("bandwidth") && root["bandwidth"].isObject()) {
        config.du.bandwidthTraceFile = getString(root["bandwidth"], "trace_file", "");
        LOG_MODULE_DEBUG(MODULE_PARSER,"Bandwidth trace file: " << (config.du.bandwidthTraceFile.empty() ? "not specified" : config.du.bandwidthTraceFile));
        
        config.du.fixedBandwidth = getInt(root["bandwidth"], "default_bandwidth_mbps", 10) * 1000000; // Convert Mbps to bps
        LOG_MODULE_DEBUG(MODULE_PARSER,"Fixed bandwidth: " << (config.du.fixedBandwidth / 1000000) << " Mbps");
    } else {
        LOG_MODULE_DEBUG(MODULE_PARSER,"No 'bandwidth' section found, using defaults");
    }
    
    // Initialize RIC with defaults first
    config.ric.ric_enabled = false;
    config.ric.ipAddress = "10.123.0.1";
    config.ric.kpmPort = 5555;
    config.ric.rcPort = 5556;
    config.ric.localKpmPort = 6555;
    config.ric.localRcPort = 6556;
    config.ric.kpm_report_interval_ms = 1000;
    
    LOG_MODULE_DEBUG(MODULE_PARSER,"Setting up RIC configuration for linkId: " << linkId);
    
    // First set up CU RIC config (for sender, linkId=0)
    bool cuRicFound = false;
    if (root.isMember("cu_ric") && root["cu_ric"].isObject()) {
        const Json::Value& cuRic = root["cu_ric"];
        config.ric.ric_enabled = getBool(cuRic, "enabled", false);
        config.ric.ipAddress = getString(cuRic, "ip", "10.123.0.1");
        config.ric.kpmPort = getInt(cuRic, "kpm_port", 5555);
        config.ric.rcPort = getInt(cuRic, "rc_port", 5556);
        config.ric.localKpmPort = getInt(cuRic, "local_kpm_port", 6555);
        config.ric.localRcPort = getInt(cuRic, "local_rc_port", 6556);
        config.ric.kpm_report_interval_ms = getInt(cuRic, "kpm_report_interval_ms", 1000);
        
        LOG_MODULE_DEBUG(MODULE_PARSER,"Found 'cu_ric' section:");
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC enabled: " << (config.ric.ric_enabled ? "true" : "false"));
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC IP: " << config.ric.ipAddress);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM port: " << config.ric.kpmPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RC port: " << config.ric.rcPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local KPM port: " << config.ric.localKpmPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local RC port: " << config.ric.localRcPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM report interval: " << config.ric.kpm_report_interval_ms << " ms");
        
        cuRicFound = true;
    }
    // For backward compatibility, also check old "ric" section
    else if (root.isMember("ric") && root["ric"].isObject()) {
        const Json::Value& ric = root["ric"];
        config.ric.ric_enabled = getBool(ric, "enabled", false);
        config.ric.ipAddress = getString(ric, "ip", "10.123.0.1");
        config.ric.kpmPort = getInt(ric, "kpm_port", 5555);
        config.ric.rcPort = getInt(ric, "rc_port", 5556);
        config.ric.localKpmPort = getInt(ric, "local_kpm_port", 6555);
        config.ric.localRcPort = getInt(ric, "local_rc_port", 6556);
        config.ric.kpm_report_interval_ms = getInt(ric, "kpm_report_interval_ms", 1000);
        
        LOG_MODULE_DEBUG(MODULE_PARSER,"Found legacy 'ric' section:");
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC enabled: " << (config.ric.ric_enabled ? "true" : "false"));
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC IP: " << config.ric.ipAddress);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM port: " << config.ric.kpmPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  RC port: " << config.ric.rcPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local KPM port: " << config.ric.localKpmPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local RC port: " << config.ric.localRcPort);
        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM report interval: " << config.ric.kpm_report_interval_ms << " ms");
        
        cuRicFound = true;
    }
    
    if (!cuRicFound) {
        LOG_MODULE_DEBUG(MODULE_PARSER,"No CU RIC section found, using defaults");
    }
    
    // For DU links (linkId > 0), look for link-specific DU RIC configuration
    bool linkFound = false;
    bool duRicFound = false;
    
    if (linkId > 0 && 
        root.isMember("network") && root["network"].isObject() && 
        root["network"].isMember("links") && root["network"]["links"].isArray()) {
        
        LOG_MODULE_DEBUG(MODULE_PARSER,"Searching for link configuration in network.links array");
        
        const Json::Value& links = root["network"]["links"];
        for (unsigned int i = 0; i < links.size(); i++) {
            if (links[i].isMember("id") && links[i]["id"].asInt() == linkId) {
                LOG_MODULE_DEBUG(MODULE_PARSER,"Found link with id " << linkId << " at index " << i);
                linkFound = true;
                
                // Found the link config
                if (links[i].isMember("du_ric") && links[i]["du_ric"].isObject()) {
                    duRicFound = true;
                    const Json::Value& duRic = links[i]["du_ric"];
                    
                    LOG_MODULE_DEBUG(MODULE_PARSER,"Found du_ric section for link " << linkId);
                    
                    // Override RIC settings with DU-specific values
                    if (duRic.isMember("enabled")) {
                        config.ric.ric_enabled = duRic["enabled"].asBool();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC enabled: " << (config.ric.ric_enabled ? "true" : "false"));
                    }
                    
                    if (duRic.isMember("ip")) {
                        config.ric.ipAddress = duRic["ip"].asString();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC IP: " << config.ric.ipAddress);
                    }
                    
                    if (duRic.isMember("kpm_port")) {
                        config.ric.kpmPort = duRic["kpm_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM port: " << config.ric.kpmPort);
                    }
                    
                    if (duRic.isMember("rc_port")) {
                        config.ric.rcPort = duRic["rc_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RC port: " << config.ric.rcPort);
                    }
                    
                    if (duRic.isMember("local_kpm_port")) {
                        config.ric.localKpmPort = duRic["local_kpm_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local KPM port: " << config.ric.localKpmPort);
                    }
                    
                    if (duRic.isMember("local_rc_port")) {
                        config.ric.localRcPort = duRic["local_rc_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local RC port: " << config.ric.localRcPort);
                    }
                    
                    if (duRic.isMember("kpm_report_interval_ms")) {
                        config.ric.kpm_report_interval_ms = duRic["kpm_report_interval_ms"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM report interval: " << config.ric.kpm_report_interval_ms << " ms");
                    }
                    
                    LOG_MODULE_INFO(MODULE_PARSER,"Using DU-specific RIC configuration for Link " << linkId);
                    LOG_MODULE_INFO(MODULE_PARSER,"  Local KPM port: " << config.ric.localKpmPort);
                    LOG_MODULE_INFO(MODULE_PARSER,"  Local RC port: " << config.ric.localRcPort);
                }
                // For backward compatibility, also check old "ric" section in link
                else if (links[i].isMember("ric") && links[i]["ric"].isObject()) {
                    duRicFound = true;
                    const Json::Value& linkRic = links[i]["ric"];
                    
                    LOG_MODULE_DEBUG(MODULE_PARSER,"Found legacy ric section for link " << linkId);
                    
                    // Override RIC settings with link-specific values
                    if (linkRic.isMember("enabled")) {
                        config.ric.ric_enabled = linkRic["enabled"].asBool();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC enabled: " << (config.ric.ric_enabled ? "true" : "false"));
                    }
                    
                    if (linkRic.isMember("ip")) {
                        config.ric.ipAddress = linkRic["ip"].asString();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RIC IP: " << config.ric.ipAddress);
                    }
                    
                    if (linkRic.isMember("kpm_port")) {
                        config.ric.kpmPort = linkRic["kpm_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM port: " << config.ric.kpmPort);
                    }
                    
                    if (linkRic.isMember("rc_port")) {
                        config.ric.rcPort = linkRic["rc_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  RC port: " << config.ric.rcPort);
                    }
                    
                    if (linkRic.isMember("local_kpm_port")) {
                        config.ric.localKpmPort = linkRic["local_kpm_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local KPM port: " << config.ric.localKpmPort);
                    }
                    
                    if (linkRic.isMember("local_rc_port")) {
                        config.ric.localRcPort = linkRic["local_rc_port"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  Local RC port: " << config.ric.localRcPort);
                    }
                    
                    if (linkRic.isMember("kpm_report_interval_ms")) {
                        config.ric.kpm_report_interval_ms = linkRic["kpm_report_interval_ms"].asInt();
                        LOG_MODULE_DEBUG(MODULE_PARSER,"  KPM report interval: " << config.ric.kpm_report_interval_ms << " ms");
                    }
                    
                    LOG_MODULE_INFO(MODULE_PARSER,"Using legacy link-specific RIC configuration for Link " << linkId);
                }
                
                break; // Found our link, no need to continue searching
            }
        }
    }
    
    if (linkId > 0) {
        if (!linkFound) {
            LOG_MODULE_WARN(MODULE_PARSER,"Link ID " << linkId << " not found in configuration");
        } else if (!duRicFound) {
            LOG_MODULE_WARN(MODULE_PARSER,"Link ID " << linkId << " found, but no du_ric or ric section present");
        }
    }
    
    // Set PDCP reordering configuration
    if (root.isMember("pdcp") && root["pdcp"].isObject()) {
        config.receiver.reordering_timeout_ms = getInt(root["pdcp"], "reordering_timeout_ms", 300);
        LOG_MODULE_DEBUG(MODULE_PARSER,"PDCP reordering timeout: " << config.receiver.reordering_timeout_ms << " ms");
    }

    LOG_MODULE_DEBUG(MODULE_PARSER,"Configuration parsing complete for linkId: " << linkId);
    return config;
}

int ConfigParser::getTraceColumn(int linkId) const {
    // Try to find the link in the links array
    if (root.isMember("network") && root["network"].isObject() && 
        root["network"].isMember("links") && root["network"]["links"].isArray()) {
        
        const Json::Value& links = root["network"]["links"];
        for (unsigned int i = 0; i < links.size(); i++) {
            if (links[i].isMember("id") && links[i]["id"].asInt() == linkId) {
                if (links[i].isMember("trace_column") && links[i]["trace_column"].isInt()) {
                    return links[i]["trace_column"].asInt() - 1; // Convert to 0-based index
                }
                break;
            }
        }
    }
    
    // Default: use linkId as column index
    return linkId - 1; // Convert to 0-based index
}

int ConfigParser::getNumLinks() const {
    if (root.isMember("network") && root["network"].isObject()) {
        return getInt(root["network"], "num_links", 2);
    }
    return 2; // Default to 2 links
}

std::string ConfigParser::getTraceFilePath() const {
    if (root.isMember("bandwidth") && root["bandwidth"].isObject()) {
        return getString(root["bandwidth"], "trace_file", "");
    }
    return "";
}

/*
ForwarderConfig ConfigParser::createForwarderConfig(ForwarderMode mode, int linkId) const {
    ForwarderConfig config;
    
    // Set basic configuration
    config.mode = mode;
    
    if (root.isMember("pdcp") && root["pdcp"].isObject()) {
        config.folderName = getString(root["pdcp"], "log_folder", "emulator_logs");
    }
    
    config.linkId = linkId;
    
    // Set RIC configuration
    if (root.isMember("ric") && root["ric"].isObject()) {
        config.ricEnabled = getBool(root["ric"], "enabled", false);
        config.ricIp = getString(root["ric"], "ip", "10.123.0.1");
        config.ricKpmPort = getInt(root["ric"], "kpm_port", 5555);
        config.ricRcPort = getInt(root["ric"], "rc_port", 5556);
        config.localKpmPort = getInt(root["ric"], "local_kpm_port", 6555);
        config.localRcPort = getInt(root["ric"], "local_rc_port", 6556);
    }
    
    // Set PDCP/RLC configuration
    if (root.isMember("pdcp") && root["pdcp"].isObject()) {
        config.pdcpReorderTimeoutMs = getInt(root["pdcp"], "reordering_timeout_ms", 300);
    }
    
    if (root.isMember("rlc") && root["rlc"].isObject()) {
        config.rlcBufferSize = getInt(root["rlc"], "buffer_size", 1000);
        config.rlcReassemblyTimeoutMs = getInt(root["rlc"], "reassembly_timeout_ms", 100);
    }
    
    return config;
}
*/
