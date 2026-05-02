#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <fstream>

/**
 * @brief A simple asynchronous logger that writes string lines to a file
 *        from a separate worker thread.
 *
 * Usage:
 *   1) Create an AsyncLogger with a file name.
 *   2) Call logLine("some text, maybe csv") from any thread.
 *   3) The lines will be asynchronously written to the file by the logger thread.
 *   4) Destructor will cleanly stop the thread and flush remaining lines.
 */
class AsyncLogger {
public:
    /**
     * @param filename Name (or path) of the file to log to.
     *                 If the file already exists, it is appended to.
     */
    explicit AsyncLogger(const std::string& filename);

    /**
     * @brief Destructor. Safely stops the logging thread and flushes any remaining lines.
     */
    ~AsyncLogger();

    /**
     * @brief Enqueue a line to be written. Thread-safe.
     * @param line The full line (CSV or otherwise) to be written to the log.
     */
    void logLine(const std::string& line);

private:
    // Disallow copy/assign for safety
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /**
     * @brief The background thread function that handles opening the file and writing lines.
     */
    void logThreadFunc();

    std::string filename_;
    std::thread logThread_;
    std::atomic<bool> stopThread_{false};

    // Queue of lines waiting to be logged
    std::queue<std::string> logQueue_;
    std::mutex logMutex_;
    std::condition_variable logCV_;
};

#endif // INTERNAL_LOGGING_H