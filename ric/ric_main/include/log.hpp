#pragma once

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <mutex>
#include <map>
#include <string_view>

// 1. Define an enum for actual log level values in C++.
enum LogLevelNum {
    TRACE_LEVEL = 0,
    DEBUG_LEVEL = 1,
    INFO_LEVEL  = 2,
    WARN_LEVEL  = 3,
    ERROR_LEVEL = 4,
    DISABLE_ALL = 5
};

// 2. Define macros for TRACE, DEBUG, INFO, etc. so they become
//    numeric tokens the preprocessor can compare.
#define TRACE 0
#define DEBUG 1
#define INFO  2
#define WARN  3
#define ERROR 4
#define DISABLE 5

// 3. Convert the macro LOGLEVEL (passed via -DLOGLEVEL=XYZ)
//    into a compile-time numeric constant called LOGLEVEL_NUM.
#if defined(LOGLEVEL) && (LOGLEVEL == TRACE)
  constexpr int LOGLEVEL_NUM = TRACE_LEVEL;
#elif defined(LOGLEVEL) && (LOGLEVEL == DEBUG)
  constexpr int LOGLEVEL_NUM = DEBUG_LEVEL;
#elif defined(LOGLEVEL) && (LOGLEVEL == WARN)
  constexpr int LOGLEVEL_NUM = WARN_LEVEL;
#elif defined(LOGLEVEL) && (LOGLEVEL == ERROR)
  constexpr int LOGLEVEL_NUM = ERROR_LEVEL;
#else
  // Default: INFO if nothing matched or not specified.
  constexpr int LOGLEVEL_NUM = INFO_LEVEL;
#endif

// Helper: get current time as a string with milliseconds.
inline std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setw(3) << std::setfill('0') << now_ms.count();
    return ss.str();
}

static std::mutex logMutex;

// Module-specific log level management
class LogManager {
private:
    inline static std::map<std::string, int> moduleLogLevels;
    inline static int globalLogLevel = LOGLEVEL_NUM; // Runtime global log level, initialized to compile-time default
    
public:
    // Set log level for a specific module
    static void setModuleLogLevel(const std::string& module, int level) {
        std::lock_guard<std::mutex> lock(logMutex);
        moduleLogLevels[module] = level;
    }
    
    // Get the log level for a specific module
    static int getModuleLogLevel(const std::string& module) {
        std::lock_guard<std::mutex> lock(logMutex);
        auto it = moduleLogLevels.find(module);
        if (it != moduleLogLevels.end()) {
            return it->second;
        }
        return globalLogLevel; // Return runtime global level
    }
    
    // Set the global log level (runtime override)
    static void setGlobalLogLevel(int level) {
        std::lock_guard<std::mutex> lock(logMutex);
        globalLogLevel = level;
    }
    
    // Get the current global log level
    static int getGlobalLogLevel() {
        std::lock_guard<std::mutex> lock(logMutex);
        return globalLogLevel;
    }
    
    // Reset a module to use the global log level
    static void resetModuleLogLevel(const std::string& module) {
        std::lock_guard<std::mutex> lock(logMutex);
        moduleLogLevels.erase(module);
    }
    
    // Reset all modules to use the global log level
    static void resetAllModuleLevels() {
        std::lock_guard<std::mutex> lock(logMutex);
        moduleLogLevels.clear();
    }
    
    // Check if we should log at this level for this module
    static bool shouldLog(const std::string& module, int level) {
        return getModuleLogLevel(module) <= level;
    }
    
    // Convert log level name to numeric value
    static int logLevelFromString(const std::string& level) {
        if (level == "TRACE") return TRACE_LEVEL;
        if (level == "DEBUG") return DEBUG_LEVEL;
        if (level == "INFO") return INFO_LEVEL;
        if (level == "WARN") return WARN_LEVEL;
        if (level == "ERROR") return ERROR_LEVEL;
        if (level == "DISABLE") return DISABLE_ALL;
        
        // Default to INFO if invalid
        return INFO_LEVEL;
    }
    
    // Convert numeric log level to string
    static std::string logLevelToString(int level) {
        switch (level) {
            case TRACE_LEVEL: return "TRACE";
            case DEBUG_LEVEL: return "DEBUG";
            case INFO_LEVEL: return "INFO";
            case WARN_LEVEL: return "WARN";
            case ERROR_LEVEL: return "ERROR";
            case DISABLE_ALL: return "DISABLE";
            default: return "UNKNOWN";
        }
    }
};

// 4. Macros for logging with module name support
#define LOG_MODULE_TRACE(module, msg)                                  \
    do {                                                               \
        if (LogManager::shouldLog(module, TRACE_LEVEL)) {              \
            std::lock_guard<std::mutex> lock(logMutex);                \
            std::cerr << getCurrentTimestamp() << " [TRACE][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_DEBUG(module, msg)                                  \
    do {                                                               \
        if (LogManager::shouldLog(module, DEBUG_LEVEL)) {              \
            std::lock_guard<std::mutex> lock(logMutex);                \
            std::cerr << getCurrentTimestamp() << " [DEBUG][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_INFO(module, msg)                                   \
    do {                                                               \
        if (LogManager::shouldLog(module, INFO_LEVEL)) {               \
            std::lock_guard<std::mutex> lock(logMutex);                \
            std::cerr << getCurrentTimestamp() << " [INFO][" << module  \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_WARN(module, msg)                                   \
    do {                                                               \
        if (LogManager::shouldLog(module, WARN_LEVEL)) {               \
            std::lock_guard<std::mutex> lock(logMutex);                \
            std::cerr << getCurrentTimestamp() << " [WARN][" << module  \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_ERROR(module, msg)                                  \
    do {                                                               \
        if (LogManager::shouldLog(module, ERROR_LEVEL)) {              \
            std::lock_guard<std::mutex> lock(logMutex);                \
            std::cerr << getCurrentTimestamp() << " [ERROR][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

// Keep the original macros for backward compatibility
#define LOG_TRACE(msg) LOG_MODULE_TRACE("Global", msg)
#define LOG_DEBUG(msg) LOG_MODULE_DEBUG("Global", msg)
#define LOG_INFO(msg)  LOG_MODULE_INFO("Global", msg)
#define LOG_WARN(msg)  LOG_MODULE_WARN("Global", msg)
#define LOG_ERROR(msg) LOG_MODULE_ERROR("Global", msg)