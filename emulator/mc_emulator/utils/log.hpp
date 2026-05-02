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
#include <atomic>

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
#elif defined(LOGLEVEL) && (LOGLEVEL == DISABLE)
  constexpr int LOGLEVEL_NUM = DISABLE_ALL;
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

// Thread-safe mutex for logging
inline std::mutex& getLogMutex() {
    static std::mutex logMutex;
    return logMutex;
}

// Module-specific log level management
class LogManager {
private:
    inline static std::map<std::string, int> moduleLogLevels;
    inline static std::atomic<int> globalLogLevel{LOGLEVEL_NUM}; // Runtime global log level, initialized to compile-time default
    
public:
    // Set log level for a specific module
    static void setModuleLogLevel(const std::string& module, int level) {
        std::lock_guard<std::mutex> lock(getLogMutex());
        moduleLogLevels[module] = level;
    }
    
    // Get the log level for a specific module
    static int getModuleLogLevel(const std::string& module) {
        std::lock_guard<std::mutex> lock(getLogMutex());
        auto it = moduleLogLevels.find(module);
        if (it != moduleLogLevels.end()) {
            return it->second;
        }
        return globalLogLevel.load(); // Return runtime global level
    }
    
    // Set the global log level (runtime override)
    static void setGlobalLogLevel(int level) {
        globalLogLevel.store(level);
    }
    
    // Get the current global log level
    static int getGlobalLogLevel() {
        return globalLogLevel.load();
    }
    
    // Reset a module to use the global log level
    static void resetModuleLogLevel(const std::string& module) {
        std::lock_guard<std::mutex> lock(getLogMutex());
        moduleLogLevels.erase(module);
    }
    
    // Reset all modules to use the global log level
    static void resetAllModuleLevels() {
        std::lock_guard<std::mutex> lock(getLogMutex());
        moduleLogLevels.clear();
    }
    
    // Check if we should log at this level for this module
    // This respects both the compile-time and runtime limits
    static bool shouldLog(const std::string& module, int level) {
        // First check the compile-time log level - if this log is excluded at compile time,
        // we can immediately return false without checking module-specific settings
        if (level < LOGLEVEL_NUM) {
            return false;
        }
        
        // Check module-specific log level
        return getModuleLogLevel(module) <= level;
    }
    
    // Check if we should log at this level globally (no module specified)
    // This respects both the compile-time and runtime limits
    static bool shouldLog(int level) {
        // First check the compile-time log level
        if (level < LOGLEVEL_NUM) {
            return false;
        }
        
        // Then check the runtime global level
        return globalLogLevel.load() <= level;
    }
    
    // Convert log level name to numeric value
    static int logLevelFromString(const std::string& level) {
        if (level == "TRACE") return TRACE_LEVEL;
        if (level == "DEBUG") return DEBUG_LEVEL;
        if (level == "INFO") return INFO_LEVEL;
        if (level == "WARN") return WARN_LEVEL;
        if (level == "ERROR") return ERROR_LEVEL;
        if (level == "DISABLE" || level == "NONE") return DISABLE_ALL;
        
        // Default to INFO if invalid
        return INFO_LEVEL;
    }
    
    // Set log level from string
    static void setGlobalLogLevel(const std::string& level) {
        setGlobalLogLevel(logLevelFromString(level));
    }
    
    // Set module log level from string
    static void setModuleLogLevel(const std::string& module, const std::string& level) {
        setModuleLogLevel(module, logLevelFromString(level));
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
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [TRACE][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_DEBUG(module, msg)                                  \
    do {                                                               \
        if (LogManager::shouldLog(module, DEBUG_LEVEL)) {              \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [DEBUG][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_INFO(module, msg)                                   \
    do {                                                               \
        if (LogManager::shouldLog(module, INFO_LEVEL)) {               \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [INFO][" << module  \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_WARN(module, msg)                                   \
    do {                                                               \
        if (LogManager::shouldLog(module, WARN_LEVEL)) {               \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [WARN][" << module  \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

#define LOG_MODULE_ERROR(module, msg)                                  \
    do {                                                               \
        if (LogManager::shouldLog(module, ERROR_LEVEL)) {              \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [ERROR][" << module \
                      << "] " << msg << std::endl;                     \
        }                                                              \
    } while (0)

// Global logging macros (without module) - these respect both compile-time and runtime limits
#define LOG_TRACE(msg)                                                 \
    do {                                                               \
        if (LogManager::shouldLog(TRACE_LEVEL)) {                      \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [TRACE] "          \
                      << msg << std::endl;                             \
        }                                                              \
    } while (0)

#define LOG_DEBUG(msg)                                                 \
    do {                                                               \
        if (LogManager::shouldLog(DEBUG_LEVEL)) {                      \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [DEBUG] "          \
                      << msg << std::endl;                             \
        }                                                              \
    } while (0)

#define LOG_INFO(msg)                                                  \
    do {                                                               \
        if (LogManager::shouldLog(INFO_LEVEL)) {                       \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [INFO] "           \
                      << msg << std::endl;                             \
        }                                                              \
    } while (0)

#define LOG_WARN(msg)                                                  \
    do {                                                               \
        if (LogManager::shouldLog(WARN_LEVEL)) {                       \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [WARN] "           \
                      << msg << std::endl;                             \
        }                                                              \
    } while (0)

#define LOG_ERROR(msg)                                                 \
    do {                                                               \
        if (LogManager::shouldLog(ERROR_LEVEL)) {                      \
            std::lock_guard<std::mutex> lock(getLogMutex());           \
            std::cerr << getCurrentTimestamp() << " [ERROR] "          \
                      << msg << std::endl;                             \
        }                                                              \
    } while (0)

// Backward compatibility - map old global macros to new module versions
// This allows existing code to work with the new system
#ifndef DISABLE_LEGACY_LOGGING
  #define LOG_MODULE_GLOBAL(module, level, msg) LOG_MODULE_##level(module, msg)
  #define LOG_GLOBAL(level, msg) LOG_##level(msg)
#endif