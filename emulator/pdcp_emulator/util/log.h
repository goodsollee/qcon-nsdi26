#pragma once

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <mutex>

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

// 4. Macros for logging. If LOGLEVEL_NUM is above the threshold,
//    these become no-ops. Now with timestamps.
#define LOG_TRACE(msg)                                         \
    do {                                                       \
        if (LOGLEVEL_NUM <= TRACE_LEVEL) {                     \
            std::lock_guard<std::mutex> lock(logMutex); \
            std::cerr << getCurrentTimestamp() << " [TRACE] "  \
                      << msg << std::endl;                     \
        }                                                      \
    } while (0)

#define LOG_DEBUG(msg)                                         \
    do {                                                       \
        if (LOGLEVEL_NUM <= DEBUG_LEVEL) {                     \
            std::lock_guard<std::mutex> lock(logMutex); \
            std::cerr << getCurrentTimestamp() << " [DEBUG] "  \
                      << msg << std::endl;                     \
        }                                                      \
    } while (0)

#define LOG_INFO(msg) \
    do { \
        if (LOGLEVEL_NUM <= INFO_LEVEL) { \
            std::lock_guard<std::mutex> lock(logMutex); \
            std::cerr << getCurrentTimestamp() << " [INFO]  " << msg << std::endl; \
        } \
    } while (0)

#define LOG_WARN(msg)                                          \
    do {                                                       \
        if (LOGLEVEL_NUM <= WARN_LEVEL) {                      \
            std::lock_guard<std::mutex> lock(logMutex); \
            std::cerr << getCurrentTimestamp() << " [WARN]  "  \
                      << msg << std::endl;                     \
        }                                                      \
    } while (0)

#define LOG_ERROR(msg)                                         \
    do {                                                       \
        if (LOGLEVEL_NUM <= ERROR_LEVEL) {                     \
            std::lock_guard<std::mutex> lock(logMutex); \
            std::cerr << getCurrentTimestamp() << " [ERROR] "  \
                      << msg << std::endl;                     \
        }                                                      \
    } while (0)
