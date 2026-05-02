#ifndef PHY_SENDER_HPP
#define PHY_SENDER_HPP

#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <string>
#include <fstream>
#include "log.hpp"

// PHY Sender Module - Responsible for bandwidth management and transmission
class PhySenderModule {
public:
    PhySenderModule(const std::string& bwTraceFile = "", uint32_t updateIntervalMs = 100, int columnIndex = 1, const std::string& logFolder = "");
    ~PhySenderModule();
    
    // Start the bandwidth trace reading thread
    void start();
    
    // Stop the bandwidth trace reading thread
    void stop();

    void startTrace();

    // Toggle ideal mode (combined bandwidth from both links)
    void idealMode();
    
    // Process packet received from MAC layer
    bool processPacket(const unsigned char* packet, size_t len);
    
    // Get available bytes for transmission
    size_t getAvailableBytes(double slotDuration);
    
    // Set callback for delivering packets to the next hop
    void setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback);
    
    // Manually set bandwidth (useful when not using trace file)
    void setBandwidth(uint32_t bps);
    
    // Get current bandwidth
    uint32_t getCurrentBandwidth() const;
    
    // Get bytes sent
    uint32_t getBytesSent() const;
    
    // Get extra bytes sent (beyond primary capacity)
    uint32_t getExtraBytesSent() const;
    
    // Reset extra bytes counter
    void resetExtraBytesSent();
    
    // Set the column index to read from the trace file
    void setTraceColumnIndex(int columnIndex);

    // Get a file path in the log folder
    std::string getLogFilePath(const std::string& filename) const;

private:
    std::string bandwidthTraceFile;
    uint32_t updateInterval; // in milliseconds
    int traceColumnIndex;    // Which column to read from trace file (0-based, excluding time column)
    std::string logFolder;   // Folder to store logs
    std::atomic<uint32_t> currentBandwidth; // in bits per second
    std::atomic<uint32_t> primaryBandwidth{0}; // in bits per second (just the primary link)
    std::atomic<uint32_t> accumulatedBytes{0}; // bytes available for transmission
    std::atomic<uint32_t> bytesSent{0};
    std::atomic<uint32_t> extraBytesSent{0}; // Bytes sent beyond primary link capacity
    
    std::thread bwUpdateThread;
    std::atomic<bool> running{false};
    std::atomic<bool> running_trace{false};
    std::atomic<bool> idealModeEnabled{false}; // Flag to track if ideal mode is active
    std::mutex mutex;
    
    std::chrono::steady_clock::time_point lastUpdateTime;
    std::chrono::steady_clock::time_point lastTransmitTime;
    std::function<void(const unsigned char*, size_t)> deliveryCallback;
    
    // Update bandwidth based on trace file
    void updateBandwidthFromTrace();
    
    // Parse a CSV line and extract the desired column value
    double parseTraceFileLine(const std::string& line);
    
    // Get the bandwidth value at a specific time from the trace file
    double getBandwidthAtTime(uint64_t time_ms, int colIndex);
};

#endif // PHY_SENDER_HPP