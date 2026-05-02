#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <jsoncpp/json/json.h>
#include <time.h>

#include "log.hpp"

#define MODULE "EmulatorManager"

// Global atomic for clean shutdown
static std::atomic_bool g_running(true);

// A helper function that kills any process occupying the specified TCP port
void killProcessOnPort(int port)
{
    std::ostringstream oss;
    oss << "fuser -k " << port << "/tcp"; 
    // Example: "fuser -k 12000/tcp"
    // This will terminate any process listening on TCP port 12000
    std::cout << "[Manager] Forcing free of TCP port " << port << "...\n";
    system(oss.str().c_str());
}


// Process management structure
struct ProcessInfo {
    std::string name;
    pid_t pid;
    std::string bind_address;
    std::string connect_address;
    std::string log_file;
};

// Signal handler
static void signalHandler(int) {
    LOG_MODULE_INFO(MODULE, "Received signal. Shutting down...");
    g_running.store(false);
}

// Function to create child process with proper logging
pid_t startComponent(const std::string& executable, const std::vector<std::string>& args, 
                     const std::string& logFile) {

    // Build a string to show the full command (executable + args).
    std::ostringstream cmdStream;
    cmdStream << executable;
    for (const auto& arg : args) {
        cmdStream << " " << arg;
    }

    // Log it in the manager's context (parent).
    LOG_MODULE_INFO(MODULE, "startComponent: " << cmdStream.str()
                     << " [logFile=" << logFile << "]");
        
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child process
        
        // Redirect stdout and stderr to log file
        if (!logFile.empty()) {
            // Create directory if needed
            size_t lastSlash = logFile.find_last_of('/');
            if (lastSlash != std::string::npos) {
                std::string dir = logFile.substr(0, lastSlash);
                std::string mkdirCmd = "mkdir -p " + dir;
                system(mkdirCmd.c_str());
            }
            
            // Open log file
            int fd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd != -1) {
                // Redirect stdout and stderr to the log file
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        
        // Execute the component
        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(executable.c_str()));
        
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        
        execvp(executable.c_str(), c_args.data());
        
        // If execvp returns, there was an error
        std::cerr << "Failed to execute: " << executable << std::endl;
        exit(EXIT_FAILURE);
    }
    
    return pid;
}

// Function to generate a component-specific configuration
bool generateComponentConfig(const Json::Value& mainConfig, 
                            const std::string& componentName,
                            int linkIndex, 
                            int processorPort, int receiverPort, int basePort,
                            const std::string& outputPath) {
    try {
        Json::Value componentConfig;
        
        // Copy common settings
        componentConfig["version"] = "1.0";
        
        // Create PDCP configs array
        Json::Value pdcpConfigs(Json::arrayValue);
        
        // Common configuration for the component
        Json::Value componentSpecificConfig;
        componentSpecificConfig["name"] = componentName;
        
        // Common settings
        Json::Value commonConfig;
        commonConfig["logFoldername"] = mainConfig["pdcp"]["log_folder"].asString();
        commonConfig["path_num"] = mainConfig["num_links"].asInt();
        componentSpecificConfig["common"] = commonConfig;
        
        // RIC settings
        Json::Value ricConfig;
        if (componentName == "PdcpSender" && mainConfig["cu_ric"]["enabled"].asBool()) {
            ricConfig["ric_enabled"] = true;
            ricConfig["kpm_report_interval_ms"] = mainConfig["cu_ric"]["kpm_report_interval_ms"].asInt();
        } 
        else if ((componentName == "PdcpDULink" || componentName == "PdcpUELink") && 
                 linkIndex < mainConfig["network"]["links"].size() &&
                 mainConfig["network"]["links"][linkIndex]["du_ric"]["enabled"].asBool()) {
            ricConfig["ric_enabled"] = true;
            ricConfig["kpm_report_interval_ms"] = 
                mainConfig["network"]["links"][linkIndex]["du_ric"]["kpm_report_interval_ms"].asInt();
        } 
        else {
            ricConfig["ric_enabled"] = false;
            ricConfig["kpm_report_interval_ms"] = 1000;
        }
        componentSpecificConfig["ric"] = ricConfig;
        
        // DU settings
        Json::Value duConfig;
        duConfig["linkId"] = linkIndex;  // Preserves the linkId from config

        // If a specific linkId is provided in the original config, use it
        if (linkIndex < mainConfig["network"]["links"].size() && 
            mainConfig["network"]["links"][linkIndex].isMember("id")) {
            duConfig["linkId"] = mainConfig["network"]["links"][linkIndex]["id"].asInt();
        }

        // Set trace column if specified
        if (linkIndex < mainConfig["network"]["links"].size() && 
            mainConfig["network"]["links"][linkIndex].isMember("trace_column")) {
            duConfig["trace_column"] = mainConfig["network"]["links"][linkIndex]["trace_column"].asInt();
        }

        duConfig["bufferSize"] = mainConfig["rlc"]["buffer_size"].asInt();
        
        // Use bandwidth from trace file or default
        if (mainConfig["bandwidth"].isMember("default_bandwidth_mbps")) {
            duConfig["fixedBandwidth"] = 
                static_cast<int>(mainConfig["bandwidth"]["default_bandwidth_mbps"].asDouble() * 1000000);
        } else {
            duConfig["fixedBandwidth"] = 10000000;  // 10 Mbps default
        }
        
        duConfig["bwUpdateInterval"] = 100;  // Default value
        duConfig["retransmission_timeout_ms"] = mainConfig["rlc"]["retransmission_timeout_ms"].asInt();
        duConfig["max_retransmissions"] = mainConfig["rlc"]["max_retransmissions"].asInt();
        duConfig["poll_retransmit_timer"] = mainConfig["rlc"]["poll_retransmit_timer_ms"].asInt();
        componentSpecificConfig["du"] = duConfig;
        
        // Receiver settings
        Json::Value receiverConfig;
        receiverConfig["reordering_timeout_ms"] = mainConfig["pdcp"]["reordering_timeout_ms"].asInt();
        componentSpecificConfig["receiver"] = receiverConfig;
        
        // Add the component config to the array
        pdcpConfigs.append(componentSpecificConfig);
        
        // Set the configs in the main object
        componentConfig["pdcp_configs"] = pdcpConfigs;
        
        // Add ZMQ link definitions based on component type
        Json::Value links(Json::arrayValue);

        int senderBasePorts = basePort;                   // PDCP Sender -> DU Links
        int duBasePort = senderBasePorts + 1000;          // DU Links -> UE Links
        int ueBasePort = duBasePort + 1000;               // UE Links -> PDCP Receiver
    
        if (componentName == "PdcpSender") {
            // PDCP Processor -> PDCP Sender link
            Json::Value procToSender;
            procToSender["id"] = 1;
            procToSender["name"] = "ProcessorToSender";
            procToSender["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(processorPort);
            procToSender["destinationEndpoint"] = "tcp://*:" + std::to_string(processorPort);
            links.append(procToSender);
            
            // Add a link for each path (PDCP Sender -> DU Link)
            for (int i = 0; i < mainConfig["num_links"].asInt(); i++) {
                Json::Value senderToDU;
                senderToDU["id"] = i + 2;
                senderToDU["name"] = "SenderToDU_" + std::to_string(i);
                senderToDU["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(senderBasePorts + i);
                senderToDU["destinationEndpoint"] = "tcp://*:" + std::to_string(senderBasePorts + i);
                links.append(senderToDU);
            }
        } 
        else if (componentName == "PdcpDULink") {
            // PDCP Sender -> DU Link
            Json::Value senderToDU;
            senderToDU["id"] = 1;
            senderToDU["name"] = "SenderToDU_" + std::to_string(linkIndex);
            senderToDU["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(senderBasePorts + linkIndex);
            senderToDU["destinationEndpoint"] = "tcp://*:" + std::to_string(senderBasePorts + linkIndex);
            links.append(senderToDU);
            
            // DU Link -> UE Link
            Json::Value duToUE;
            duToUE["id"] = 2;
            duToUE["name"] = "DUToUE_" + std::to_string(linkIndex);
            duToUE["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(duBasePort + linkIndex);
            duToUE["destinationEndpoint"] = "tcp://*:" + std::to_string(duBasePort + linkIndex);
            links.append(duToUE);
        } 
        else if (componentName == "PdcpUELink") {
            // DU Link -> UE Link
            Json::Value duToUE;
            duToUE["id"] = 1;
            duToUE["name"] = "DUToUE_" + std::to_string(linkIndex);
            duToUE["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(duBasePort + linkIndex);
            duToUE["destinationEndpoint"] = "tcp://*:" + std::to_string(duBasePort + linkIndex);
            links.append(duToUE);
            
            // UE Link -> PDCP Receiver
            Json::Value ueToReceiver;
            ueToReceiver["id"] = 2;
            ueToReceiver["name"] = "UEToReceiver_" + std::to_string(linkIndex);
            ueToReceiver["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(ueBasePort + linkIndex);
            ueToReceiver["destinationEndpoint"] = "tcp://*:" + std::to_string(ueBasePort + linkIndex);
            links.append(ueToReceiver);
        } 
        else if (componentName == "PdcpReceiver") {
            // Add all UE Link -> PDCP Receiver links
            for (int i = 0; i < mainConfig["num_links"].asInt(); i++) {
                Json::Value ueToReceiver;
                ueToReceiver["id"] = i + 1;
                ueToReceiver["name"] = "UEToReceiver_" + std::to_string(i);
                ueToReceiver["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(ueBasePort + i);
                ueToReceiver["destinationEndpoint"] = "tcp://*:" + std::to_string(ueBasePort + i);
                links.append(ueToReceiver);
            }
            
            // PDCP Receiver -> PDCP Processor
            Json::Value receiverToProc;
            receiverToProc["id"] = mainConfig["num_links"].asInt() + 1;
            receiverToProc["name"] = "ReceiverToProcessor";
            receiverToProc["sourceEndpoint"] = "tcp://127.0.0.1:" + std::to_string(receiverPort);
            receiverToProc["destinationEndpoint"] = "tcp://*:" + std::to_string(receiverPort);
            links.append(receiverToProc);
        }
        
        componentConfig["links"] = links;
        
        // Write to file
        std::ofstream configFile(outputPath);
        if (!configFile.is_open()) {
            LOG_MODULE_ERROR(MODULE, "Failed to open config file for writing: " << outputPath);
            return false;
        }
        
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::string configStr = Json::writeString(writer, componentConfig);
        configFile << configStr;
        configFile.close();
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_MODULE_ERROR(MODULE, "Error generating component config: " << e.what());
        return false;
    }
}

// Function to print component status information
void printComponentStatus(const std::vector<ProcessInfo>& processes) {
    std::cout << "\n===== PDCP Emulator Component Status =====\n";
    std::cout << "Component                PID      Log File\n";
    std::cout << "----------------------------------------\n";
    
    for (const auto& proc : processes) {
        std::cout << std::left << std::setw(25) << proc.name 
                  << std::setw(10) << proc.pid 
                  << proc.log_file << "\n";
    }
    std::cout << "\nTo monitor logs: ./monitor_logs.sh\n";
}

int main(int argc, char* argv[]) {
    // Register signal handler
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Parse arguments
    std::string configFile = "config.json";
    int basePort = 12000;  // Default base port
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                configFile = argv[++i];
            }
        } else if (arg == "-p" || arg == "--base-port") {
            if (i + 1 < argc) {
                basePort = std::stoi(argv[++i]);
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c, --config FILE   Configuration file (default: config.json)" << std::endl;
            std::cout << "  -p, --base-port N   Base port number (default: 12000)" << std::endl;
            std::cout << "  -h, --help          Show this help message" << std::endl;
            return 0;
        }
    }
    
    // Load configuration
    LOG_MODULE_INFO(MODULE, "Loading configuration from " << configFile);
    
    Json::Value config;
    std::ifstream configStream(configFile);
    if (!configStream.is_open()) {
        LOG_MODULE_ERROR(MODULE, "Failed to open config file: " << configFile);
        return 1;
    }
    
    Json::CharReaderBuilder reader;
    std::string errs;
    if (!Json::parseFromStream(reader, configStream, &config, &errs)) {
        LOG_MODULE_ERROR(MODULE, "Failed to parse config file: " << errs);
        return 1;
    }
    
    // Create temp directory for component configs
    std::string tempDir = "/tmp/pdcp_emulator_configs";
    std::string mkdirCmd = "mkdir -p " + tempDir;
    system(mkdirCmd.c_str());
    
    // Create log directory
    std::string logDir = config["pdcp"]["log_folder"].asString();
    std::string mklogdirCmd = "mkdir -p " + logDir;
    system(mklogdirCmd.c_str());
    
    int numLinks = config["num_links"].asInt();
    LOG_MODULE_INFO(MODULE, "Setting up emulator with " << numLinks << " links");

    // Define ZMQ ports
    int processorPort = 10000;                        // PDCP Processor -> PDCP Sender
    int senderBasePorts = basePort;                   // PDCP Sender -> DU Links
    int duBasePort = senderBasePorts + 1000;          // DU Links -> UE Links
    int ueBasePort = duBasePort + 1000;               // UE Links -> PDCP Receiver
    int receiverPort = 9000;                          // PDCP Receiver -> PDCP Processor

    killProcessOnPort(processorPort);
    killProcessOnPort(receiverPort);
    // If you have multiple links, you may need to do this in a loop:
    for (int i = 0; i < numLinks; i++) {
        killProcessOnPort(basePort + i);
        killProcessOnPort(duBasePort + i);
        killProcessOnPort(ueBasePort + i);
    }

    
    // Generate component configurations
    std::string senderConfigPath = tempDir + "/pdcp_sender_config.json";
    std::string receiverConfigPath = tempDir + "/pdcp_receiver_config.json";
    std::vector<std::string> duConfigPaths;
    std::vector<std::string> ueConfigPaths;
    
    // Generate PDCP Sender config
    if (!generateComponentConfig(config, "PdcpSender", 0, processorPort, receiverPort, basePort, senderConfigPath)) {
        LOG_MODULE_ERROR(MODULE, "Failed to generate PDCP Sender config");
        return 1;
    }
    
    // Generate PDCP Receiver config
    if (!generateComponentConfig(config, "PdcpReceiver", 0, processorPort, receiverPort, basePort, receiverConfigPath)) {
        LOG_MODULE_ERROR(MODULE, "Failed to generate PDCP Receiver config");
        return 1;
    }
    
    // Generate DU and UE configs for each link
    for (int i = 0; i < numLinks; i++) {
        std::string duConfigPath = tempDir + "/pdcp_du_link_" + std::to_string(i) + "_config.json";
        std::string ueConfigPath = tempDir + "/pdcp_ue_link_" + std::to_string(i) + "_config.json";
        
        if (!generateComponentConfig(config, "PdcpDULink", i, processorPort, receiverPort, basePort, duConfigPath)) {
            LOG_MODULE_ERROR(MODULE, "Failed to generate PDCP DU Link config for link " << i);
            return 1;
        }
        
        if (!generateComponentConfig(config, "PdcpUELink", i, processorPort, receiverPort, basePort, ueConfigPath)) {
            LOG_MODULE_ERROR(MODULE, "Failed to generate PDCP UE Link config for link " << i);
            return 1;
        }
        
        duConfigPaths.push_back(duConfigPath);
        ueConfigPaths.push_back(ueConfigPath);
    }
    
    // Start components
    std::vector<ProcessInfo> processes;
    
    // Set up log files
    std::string receiverLogFile = logDir + "/pdcp_receiver.log";
    std::string senderLogFile = logDir + "/pdcp_sender.log";
    std::vector<std::string> duLogFiles;
    std::vector<std::string> ueLogFiles;
    
    for (int i = 0; i < numLinks; i++) {
        duLogFiles.push_back(logDir + "/pdcp_du_link_" + std::to_string(i) + ".log");
        ueLogFiles.push_back(logDir + "/pdcp_ue_link_" + std::to_string(i) + ".log");
    }
    
    // Start PDCP Receiver first
    {
        LOG_MODULE_INFO(MODULE, "Starting PDCP Receiver...");
        // Suppose the receiver’s “listen” port is `receiverPort` (which we set to 9000).
        // We'll pass both -c and -r to the receiver.
        std::vector<std::string> receiverArgs = {
            "-r", std::to_string(ueBasePort), // Processor->Receiver
            "-s", std::to_string(receiverPort)    // e.g. 9000
        };

        pid_t receiverPid = startComponent("./bin/pdcp_receiver", receiverArgs, receiverLogFile);
        if (receiverPid <= 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to start PDCP Receiver");
            return 1;
        }
        processes.push_back({"PDCP Receiver", receiverPid, "", "", receiverLogFile});

        // Give it time to initialize
        sleep(1);
    }

    // Start UE Links
    for (int i = 0; i < numLinks; i++) {
        LOG_MODULE_INFO(MODULE, "Starting PDCP UE Link " << i << "...");

        // Suppose the UE link receives from DU at (duBasePort + i)
        // and sends to the Receiver at (ueBasePort + i).
        // Also pass "-i linkID" to identify which link we are.
        int ueReceivePort = duBasePort + i;  // e.g. 13000 + i
        int ueSendPort    = ueBasePort;  // e.g. 14000 + i

        std::vector<std::string> ueArgs = {
            "-c", configFile,
            "-r", std::to_string(ueReceivePort),  // DU->UE
            "-s", std::to_string(ueSendPort),     // UE->Receiver
            "-i", std::to_string(i)
        };

        pid_t uePid = startComponent("./bin/pdcp_ue_link", ueArgs, ueLogFiles[i]);
        if (uePid <= 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to start PDCP UE Link " << i);
            return 1;
        }
        
        processes.push_back({"PDCP UE Link " + std::to_string(i), uePid, "", "", ueLogFiles[i]});
        usleep(500000);  // short delay
    }

    // Start DU Links
    for (int i = 0; i < numLinks; i++) {
        LOG_MODULE_INFO(MODULE, "Starting PDCP DU Link " << i << "...");

        // Suppose the DU link receives from Sender at (senderBasePorts + i)
        // and sends to UE at (duBasePort + i).
        // Also pass "-i linkID".
        int duReceivePort = senderBasePorts + i; // e.g. 12000 + i
        int duSendPort    = duBasePort + i;      // e.g. 13000 + i

        std::vector<std::string> duArgs = {
            "-c", configFile,
            "-r", std::to_string(duReceivePort),  // Sender->DU
            "-s", std::to_string(duSendPort),     // DU->UE
            "-i", std::to_string(i)
        };

        pid_t duPid = startComponent("./bin/pdcp_du_link", duArgs, duLogFiles[i]);
        if (duPid <= 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to start PDCP DU Link " << i);
            return 1;
        }
        
        processes.push_back({"PDCP DU Link " + std::to_string(i), duPid, "", "", duLogFiles[i]});
        usleep(500000);
    }

    // Finally, start PDCP Sender
    {
        LOG_MODULE_INFO(MODULE, "Starting PDCP Sender...");

        // Suppose the Sender has to connect with the Processor at `processorPort` (10000).
        // We can also pass the basePort if needed, or treat it as “-s”.
        std::vector<std::string> senderArgs = {
            "-c", configFile,
            "-r", std::to_string(processorPort), // Processor->Sender
            "-s", std::to_string(senderBasePorts) // base port for DU
        };

        pid_t senderPid = startComponent("./bin/pdcp_sender", senderArgs, senderLogFile);
        if (senderPid <= 0) {
            LOG_MODULE_ERROR(MODULE, "Failed to start PDCP Sender");
            return 1;
        }

        processes.push_back({"PDCP Sender", senderPid, "", "", senderLogFile});
    }

    
    // Wait for all processes to initialize
    sleep(2);
    
    LOG_MODULE_INFO(MODULE, "All components started successfully!");
    
    // Display component status
    std::cout << "\n===== PDCP Emulator Component Status =====\n";
    std::cout << "Component                PID      Log File\n";
    std::cout << "----------------------------------------\n";
    
    for (const auto& proc : processes) {
        std::cout << std::left << std::setw(25) << proc.name 
                  << std::setw(10) << proc.pid 
                  << proc.log_file << "\n";
    }
    
    LOG_MODULE_INFO(MODULE, "Press Ctrl+C to stop the emulator");
    
    // Main loop - keep running until signal handler sets g_running to false
    while (g_running.load()) {
        // Check if any child process has terminated
        for (auto& proc : processes) {
            int status;
            pid_t result = waitpid(proc.pid, &status, WNOHANG);
            
            if (result > 0) {
                // Child process terminated
                if (WIFEXITED(status)) {
                    LOG_MODULE_WARN(MODULE, proc.name << " exited with status " << WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    LOG_MODULE_WARN(MODULE, proc.name << " killed by signal " << WTERMSIG(status));
                }
                
                // Set running to false to terminate all other processes
                g_running.store(false);
                break;
            }
        }
        
        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Terminate all child processes
    LOG_MODULE_INFO(MODULE, "Shutting down all components...");
    
    for (auto& proc : processes) {
        LOG_MODULE_INFO(MODULE, "Terminating " << proc.name);
        kill(proc.pid, SIGTERM);
    }
    
    // Wait for all processes to terminate with a timeout
    int timeoutSeconds = 5;
    time_t startTime = time(nullptr);
    bool allTerminated = false;
    
    while (!allTerminated && (time(nullptr) - startTime) < timeoutSeconds) {
        allTerminated = true;
        
        for (auto& proc : processes) {
            int status;
            pid_t result = waitpid(proc.pid, &status, WNOHANG);
            
            if (result == 0) {
                // Process still running
                allTerminated = false;
            }
        }
        
        if (!allTerminated) {
            // Sleep briefly before checking again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // Force kill any remaining processes
    for (auto& proc : processes) {
        int status;
        pid_t result = waitpid(proc.pid, &status, WNOHANG);
        
        if (result == 0) {
            // Process still running, force kill
            LOG_MODULE_WARN(MODULE, proc.name << " did not terminate gracefully, sending SIGKILL");
            kill(proc.pid, SIGKILL);
            waitpid(proc.pid, &status, 0);
        }
    }
    
    LOG_MODULE_INFO(MODULE, "PDCP Emulator Manager terminated gracefully");
    return 0;
}