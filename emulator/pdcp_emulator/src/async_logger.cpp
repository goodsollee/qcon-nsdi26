#include "async_logger.h"
#include "log.h"

#include <iostream>  // For std::cerr, if needed

AsyncLogger::AsyncLogger(const std::string& filename)
    : filename_(filename)
{
    // Start the background logging thread
    logThread_ = std::thread(&AsyncLogger::logThreadFunc, this);
}

AsyncLogger::~AsyncLogger()
{
    // Signal the thread to stop
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        stopThread_ = true;
    }
    logCV_.notify_one();

    // Join the thread to ensure clean shutdown
    if (logThread_.joinable()) {
        logThread_.join();
    }
}

void AsyncLogger::logLine(const std::string& line)
{
    // Push the line onto the queue
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        logQueue_.push(line);
    }
    // Notify the logging thread
    logCV_.notify_one();
}

void AsyncLogger::logThreadFunc()
{
    // Open file in append mode
    std::ofstream outFile(filename_, std::ios::out | std::ios::app);
    if (!outFile.is_open()) {
        // If needed, handle error
        LOG_ERROR("[AsyncLogger] Error: Could not open file " 
                  << filename_ << " for logging.");
        return;
    }

    // (Optional) write a header line if you want each run to start with a CSV header
    // outFile << "time_ms,pdcp_seq,tcp_seq\n";

    while (true) {
        std::unique_lock<std::mutex> lock(logMutex_);
        // Wait until we have lines to write or are stopping
        logCV_.wait(lock, [this]() {
            return !logQueue_.empty() || stopThread_;
        });

        // If stopThread_ is true and no lines remain, exit the loop
        if (stopThread_ && logQueue_.empty()) {
            break;
        }

        // Drain the queue
        while (!logQueue_.empty()) {
            outFile << logQueue_.front() << "\n";
            logQueue_.pop();
        }

        // Flush to ensure data hits disk promptly
        outFile.flush();
    }

    // File automatically closed on ofstream destruction
}
