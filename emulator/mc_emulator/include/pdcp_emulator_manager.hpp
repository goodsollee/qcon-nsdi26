#ifndef PDCP_EMULATOR_MANAGER_HPP
#define PDCP_EMULATOR_MANAGER_HPP

#include <string>
#include <vector>
#include <atomic>
#include <jsoncpp/json/json.h>

// Process management structure
struct ProcessInfo {
    std::string name;
    pid_t pid;
    std::string bind_address;
    std::string connect_address;
};

class PdcpEmulatorManager {
public:
    PdcpEmulatorManager();
    ~PdcpEmulatorManager();
    
    // Initialize the manager with config file and base port
    bool initialize(const std::string& configFile, int basePort);
    
    // Start all components
    bool startComponents();
    
    // Stop all components
    void stopComponents();
    
    // Main run loop
    void run();
    
    // Signal handler callback
    void handleSignal();

private:
    // Load configuration from file
    bool loadConfig(const std::string& configFile);
    
    // Generate component-specific configuration
    bool generateComponentConfig(const std::string& componentName, 
                                int linkIndex, 
                                const std::string& outputPath);
    
    // Start a specific component
    pid_t startComponent(const std::string& executable, 
                         const std::vector<std::string>& args);
    
    // Check if all processes are still running
    bool checkProcesses();

private:
    // Main configuration
    Json::Value config_;
    
    // Base port for ZMQ communication
    int basePort_;
    
    // Number of links
    int numLinks_;
    
    // List of running processes
    std::vector<ProcessInfo> processes_;
    
    // Component config paths
    std::string senderConfigPath_;
    std::string receiverConfigPath_;
    std::vector<std::string> duConfigPaths_;
    std::vector<std::string> ueConfigPaths_;
    
    // Temp directory for configs
    std::string tempDir_;
    
    // Running flag
    std::atomic_bool running_;
};

#endif // PDCP_EMULATOR_MANAGER_HPP