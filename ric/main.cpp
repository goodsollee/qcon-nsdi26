#include "RIC.h"
#include <iostream>
#include <string>

static const char* MAIN_MODULE = "main";  // Define module name for logging

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --log-level=LEVEL   Set log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)" << std::endl;
    std::cout << "  --help              Show this help message" << std::endl;
}

LogLevel parse_log_level(const std::string& level) {
    if (level == "DEBUG") return LogLevel::DEBUG;
    if (level == "INFO") return LogLevel::INFO;
    if (level == "WARNING") return LogLevel::WARNING;
    if (level == "ERROR") return LogLevel::ERROR;
    if (level == "CRITICAL") return LogLevel::CRITICAL;
    return LogLevel::INFO;  // Default
}

int main(int argc, char* argv[]) {
    // Default log level
    LogLevel log_level = LogLevel::INFO;
    bool show_help = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--log-level=", 0) == 0) {
            std::string level = arg.substr(12); // Length of "--log-level="
            log_level = parse_log_level(level);
            std::cout << "Setting log level to: " << level << std::endl;
        } else if (arg == "--help" || arg == "-h") {
            show_help = true;
        }
    }

    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // Initialize logger with specified level
    Logger::getInstance().setLogLevel(log_level);

    // Filter module logging
    Logger::getInstance().setModuleFilter("all");
    
    try {
        // Create RIC instance using the factory method
        auto ric = RANIntelligentController::create("tcp://localhost:7878");
        if (!ric) {
            LOG_CRITICAL(MAIN_MODULE, "Failed to create RIC instance");
            return 1;
        }

        // Initialize RIC
        if (!ric->initialize()) {
            LOG_CRITICAL(MAIN_MODULE, "Failed to initialize RIC");
            return 1;
        }

        // Run RIC
        ric->run();
    } catch (const std::exception& e) {
        LOG_CRITICAL(MAIN_MODULE, "RIC initialization failed: ", e.what());
        return 1;
    }

    return 0;
}