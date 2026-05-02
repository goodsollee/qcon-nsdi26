#include "phy_sender.hpp"
#include "rlc_segmentation.hpp"
#include "pdcp_common.hpp"
#include <csignal>   // raise
#include <cstdlib>   // exit
#include <iostream>
#include <filesystem> // For directory operations

PhySenderModule::PhySenderModule(const std::string& bwTraceFile, uint32_t updateIntervalMs, int columnIndex, const std::string& logFolder) 
    : bandwidthTraceFile(bwTraceFile),
      updateInterval(updateIntervalMs),
      traceColumnIndex(columnIndex),
      logFolder(logFolder),
      currentBandwidth(100000000), // NSDI-era default (100 Mbps) — reverted from 0 to fix BWE bootstrap rate-limit starvation
      accumulatedBytes(0),
      bytesSent(0),
      running(false),
      running_trace(false),
      lastUpdateTime(std::chrono::steady_clock::now()),
      lastTransmitTime(std::chrono::steady_clock::now()) {
    
    LOG_INFO("[PHY Sender] Initialized with default bandwidth: " << (currentBandwidth.load() / 1000000.0) 
             << " Mbps, update interval: " << updateIntervalMs << " ms");
    
    // NOTE: trace replay is NOT auto-started here anymore. The RIC sends a
    // bootstrap "start_trace" (or "ideal") command on its first iteration,
    // which calls startTrace(). Auto-starting in the constructor caused the
    // usage CSV header to be written under the default (non-ideal) mode before
    // the ideal toggle could land — so the file format ended up as "time,usage"
    // instead of "time,primary_usage,secondary_usage". Deferring lets the RIC
    // command set idealModeEnabled first, before the first trace tick.
    if (!bandwidthTraceFile.empty()) {
        LOG_INFO("[PHY Sender] Using bandwidth trace file: " << bandwidthTraceFile
                 << ", column index: " << traceColumnIndex
                 << " (deferred — trace starts on RIC command)");
    }
    
    // Create log directory if it doesn't exist
    if (!logFolder.empty()) {
        try {
            std::filesystem::create_directories(logFolder);
            LOG_INFO("[PHY Sender] Log folder created/verified: " << logFolder);
        } catch (const std::exception& e) {
            LOG_ERROR("[PHY Sender] Failed to create log folder: " << logFolder << " - " << e.what());
        }
    }
}

PhySenderModule::~PhySenderModule() {
    stop();
    LOG_INFO("[PHY Sender] Cleaned up. Bytes sent: " << bytesSent.load());
}

void PhySenderModule::start() {
    if (running.load()) return;
    
    running.store(true);
}

void PhySenderModule::stop() {
    if (!running.load()) return;
    
    running.store(false);
    
    if (bwUpdateThread.joinable()) {
        bwUpdateThread.join();
    }
    LOG_INFO("[PHY Sender] Bandwidth update thread stopped");
}

void PhySenderModule::startTrace () {
    if (running_trace.load()) return;

    lastUpdateTime = std::chrono::steady_clock::now();
    lastTransmitTime = lastUpdateTime;

    // Only start the trace reading thread if a trace file is provided
    if (!bandwidthTraceFile.empty()) {
        bwUpdateThread = std::thread(&PhySenderModule::updateBandwidthFromTrace, this);
        running_trace = true;
        LOG_ERROR("[PHY Sender] Bandwidth update thread started");
    }
}

void PhySenderModule::idealMode() {
    // Toggle ideal mode
    bool enabled = idealModeEnabled.load();
    idealModeEnabled.store(!enabled);
    LOG_INFO(std::string("[PHY Sender][IdealMode] Ideal mode ")
             + (enabled ? "disabled" : "enabled"));

    startTrace ();
}


bool PhySenderModule::processPacket(const unsigned char* packet, size_t len) {
    if (!packet || len == 0) {
        return false;
    }

    // Calculate available primary bandwidth
    double slotDuration = static_cast<double>(updateInterval) / 1000.0; // convert ms to seconds
    
    // Calculate available primary bandwidth
    uint32_t pb = primaryBandwidth.load(std::memory_order_relaxed);
    if (pb == 0) {
        LOG_WARN("[PHY Sender] primaryBandwidth is 0 – skipping ideal‑mode accounting");
    }
    double primaryAvailDbl = (slotDuration * pb) / 8.0;
    size_t primaryAvailableBytes = static_cast<size_t>(primaryAvailDbl);
    if (primaryAvailableBytes == 0) primaryAvailableBytes = 1;  // 최소 1 바이트 보장
    
    // Update the bytesSent counter
    uint32_t totalBytesSent = bytesSent.fetch_add(len, std::memory_order_relaxed) + len;
    
    // Check if we're sending more than the primary link could handle
    if (idealModeEnabled.load()) {
        // Calculate if this packet exceeds the primary link's capacity
        size_t currentInterval = totalBytesSent / primaryAvailableBytes;
        size_t previousInterval = (totalBytesSent - len) / primaryAvailableBytes;
        
        // If we're still in the same interval, check if we've exceeded primary capacity
        if (currentInterval == previousInterval) {
            size_t bytesInCurrentInterval = totalBytesSent % primaryAvailableBytes;
            size_t previousBytesInInterval = (totalBytesSent - len) % primaryAvailableBytes;
            
            // If we went from below capacity to above capacity, or were already above
            if (previousBytesInInterval < primaryAvailableBytes && bytesInCurrentInterval >= primaryAvailableBytes) {
                // We exceeded primary capacity with this packet
                size_t excessBytes = bytesInCurrentInterval - previousBytesInInterval;
                extraBytesSent.fetch_add(excessBytes, std::memory_order_relaxed);
                LOG_DEBUG("[PHY Sender][IdealMode] Packet exceeded primary capacity by " 
                         << excessBytes << " bytes, total extra: " << extraBytesSent.load());
            } else if (previousBytesInInterval >= primaryAvailableBytes) {
                // We were already above capacity
                extraBytesSent.fetch_add(len, std::memory_order_relaxed);
                LOG_DEBUG("[PHY Sender][IdealMode] Packet used extra capacity: " 
                         << len << " bytes, total extra: " << extraBytesSent.load());
            }
        } else {
            // We've moved to a new interval
            // Calculate excess in the previous interval
            size_t remainingPreviousInterval = primaryAvailableBytes - ((totalBytesSent - len) % primaryAvailableBytes);
            if (remainingPreviousInterval < len) {
                // Part of the packet goes beyond the previous interval
                size_t excessBytes = len - remainingPreviousInterval;
                extraBytesSent.fetch_add(excessBytes, std::memory_order_relaxed);
                LOG_DEBUG("[PHY Sender][IdealMode] Packet crossed intervals, excess: " 
                         << excessBytes << " bytes, total extra: " << extraBytesSent.load());
            }
        }
    }

    // Forward packet to the next hop
    if (deliveryCallback) {
        deliveryCallback(packet, len);
    }
    
    LOG_DEBUG("[PHY Sender] Sent packet of size " << len << " bytes, total bytes: " << totalBytesSent);
    
    return true;
}

size_t PhySenderModule::getAvailableBytes(double slotDuration) {
    std::lock_guard<std::mutex> lock(mutex);
    
    // Calculate available bytes based on current bandwidth and slot duration
    size_t availableBytes = (slotDuration * currentBandwidth.load()) / 8 / 1000;
    
    //LOG_DEBUG("[PHY Sender] Available bytes: " << availableBytes << " for slot duration: " << slotDuration << "ms");
    
    return availableBytes;
}

void PhySenderModule::setDeliveryCallback(std::function<void(const unsigned char*, size_t)> callback) {
    deliveryCallback = callback;
}

void PhySenderModule::setBandwidth(uint32_t bps) {
    std::lock_guard<std::mutex> lock(mutex);
    
    // If in ideal mode, track the primary bandwidth separately
    if (idealModeEnabled.load()) {
        // If we're setting the combined bandwidth, we keep track of primary separately
        // We don't update primaryBandwidth here
    } else {
        // In normal mode, update both current and primary bandwidth
        primaryBandwidth.store(bps, std::memory_order_relaxed);
    }
    
    currentBandwidth.store(bps, std::memory_order_relaxed);
    LOG_INFO("[PHY Sender] Bandwidth set to " << (bps / 1000) << " kbps");
}

uint32_t PhySenderModule::getExtraBytesSent() const {
    return extraBytesSent.load(std::memory_order_relaxed);
}

void PhySenderModule::resetExtraBytesSent() {
    extraBytesSent.store(0, std::memory_order_relaxed);
    LOG_DEBUG("[PHY Sender][IdealMode] Reset extra bytes counter");
}

uint32_t PhySenderModule::getCurrentBandwidth() const {
    return currentBandwidth.load(std::memory_order_relaxed);
}

uint32_t PhySenderModule::getBytesSent() const {
    return bytesSent.load(std::memory_order_relaxed);
}

void PhySenderModule::setTraceColumnIndex(int columnIndex) {
    if (columnIndex < 0) {
        LOG_ERROR("[PHY Sender] Invalid column index: " << columnIndex << ". Using default column index 1");
        columnIndex = 1;
    }
    traceColumnIndex = columnIndex;
    LOG_INFO("[PHY Sender] Trace column index set to: " << traceColumnIndex);
}

std::string PhySenderModule::getLogFilePath(const std::string& filename) const {
    if (logFolder.empty()) {
        return filename; // Use current directory if no log folder specified
    }
    
    // Ensure path has trailing slash
    std::string folder = logFolder;
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') {
        folder += '/';
    }
    
    return folder + filename;
}

double PhySenderModule::parseTraceFileLine(const std::string& line) {
    // Skip empty lines or comments
    if (line.empty() || line[0] == '#') {
        return -1.0;
    }
    
    // Skip header line (contains column names)
    if (line.find("time") != std::string::npos && line.find(",") != std::string::npos) {
        return -1.0;
    }
    
    try {
        // Split by comma
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(line);
        
        while (std::getline(tokenStream, token, ',')) {
            tokens.push_back(token);
        }
        
        // Check if we have enough columns
        if (tokens.size() <= traceColumnIndex) {
            LOG_ERROR("[PHY Sender] Not enough columns in line: " << line 
                     << ", requested column: " << traceColumnIndex);
            return -1.0;
        }
        
        // Parse the desired column value (traceColumnIndex + 1 because first column is time)
        double value = std::stod(tokens[traceColumnIndex + 1]);
        return value;
    } catch (const std::exception& e) {
        LOG_ERROR("[PHY Sender] Error parsing line: " << line << ", error: " << e.what());
        return -1.0;
    }
}

double PhySenderModule::getBandwidthAtTime(uint64_t time_ms, int colIndex) {
    std::ifstream file(bandwidthTraceFile);
    if (!file.is_open()) {
        LOG_ERROR("[PHY Sender] Error: Could not open trace file: " << bandwidthTraceFile);
        return -1.0;
    }
    // 헤더 건너뛰기
    std::string header; std::getline(file, header);

    std::string line; double last_value = 0.0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0]=='#') continue;
        std::istringstream ss(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (std::getline(ss, tok, ',')) tokens.push_back(tok);
        if ((int)tokens.size() <= colIndex+1) continue;
        uint64_t t = std::stoull(tokens[0]);
        double v = std::stod(tokens[colIndex+1]);
        last_value = v;
        if (t == time_ms) return v;
    }
    return last_value;
}

void PhySenderModule::updateBandwidthFromTrace() {
    if (bandwidthTraceFile.empty()) {
        LOG_ERROR("[PHY Sender] No bandwidth trace file specified");
        return;
    }
    
    // First validate the trace file format
    std::ifstream check_file(bandwidthTraceFile);
    if (!check_file.is_open()) {
        LOG_ERROR("[PHY Sender] Error: Could not open bandwidth trace file: " << bandwidthTraceFile);
        return;
    }
    
    // Check header line
    std::string header;
    if (std::getline(check_file, header)) {
        if (header.find("time") == std::string::npos || header.find(",") == std::string::npos) {
            LOG_WARN("[PHY Sender] Trace file may not have the expected format. Header: " << header);
        }
    } else {
        LOG_ERROR("[PHY Sender] Empty trace file");
        return;
    }
    
    // Read all time entries from the file to determine exact trace times
    std::vector<uint64_t> trace_times;
    std::string line;
    
    while (std::getline(check_file, line)) {
        // Skip empty lines or comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
            
        // Get the time entry
        try {
            std::istringstream iss(line);
            std::string time_str;
            std::getline(iss, time_str, ',');
            uint64_t time_ms = std::stoll(time_str);
            trace_times.push_back(time_ms);
        } catch (const std::exception& e) {
            LOG_ERROR("[PHY Sender] Error parsing time from line: " << line << " - " << e.what());
        }
    }
    
    // Close check file
    check_file.close();
    
    if (trace_times.empty()) {
        LOG_ERROR("[PHY Sender] No valid time entries found in trace file");
        return;
    }
    
    LOG_INFO("[PHY Sender] Loaded " << trace_times.size() << " time entries from trace file");
    
    // Record start time for synchronized updates
    // Use a static shared start time across all PHY instances to ensure synchronized bandwidth changes
    static std::mutex start_time_mutex;
    static std::atomic<bool> global_start_time_set(false);
    static std::chrono::steady_clock::time_point global_start_time;
    
    // Acquire the start time in a thread-safe manner
    {
        std::lock_guard<std::mutex> lock(start_time_mutex);
        if (!global_start_time_set.load()) {
            global_start_time = std::chrono::steady_clock::now();
            global_start_time_set.store(true);
            LOG_INFO("[PHY Sender] Setting global start time for all links");
        }
    }
    
    auto start_time = global_start_time;
    auto start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        start_time.time_since_epoch()).count();
    size_t current_index = 0; // Current index in the trace file
    uint64_t last_trace_time = trace_times.back(); // Last time in the trace

    LOG_INFO("[PHY Sender] Start time for link " << traceColumnIndex <<" is "<< start_time_ms << "ms (synchronized)");
    
    // Get the trace interval by looking at the difference between timestamps
    uint64_t trace_interval = 0;
    if (trace_times.size() >= 2) {
        trace_interval = trace_times[1] - trace_times[0];
        LOG_INFO("[PHY Sender] Detected trace interval: " << trace_interval << "ms");
    } else {
        // Default to updateInterval if only one entry
        trace_interval = updateInterval;
        LOG_INFO("[PHY Sender] Using default trace interval: " << trace_interval << "ms");
    }
    
    uint32_t prevTotalBytes = bytesSent.load();       // 누적 전송 바이트(전체)
    uint32_t prevExtraBytes = extraBytesSent.load();  // 누적 초과 전송 바이트(secondary)
    
    // Generate log filename with path
    std::string csvFilename = getLogFilePath("usage_link" + std::to_string(traceColumnIndex) + ".csv");
    std::ofstream csvLog(csvFilename, std::ios::out);
    
    if (!csvLog.is_open()) {
        LOG_ERROR("[PHY Sender] Failed to open log file: " << csvFilename);
    } else {
        LOG_INFO("[PHY Sender] Opened log file: " << csvFilename);
    }
    
    bool headerWritten = false;

    while (running.load()) {
        try {
            // Calculate elapsed time for trace synchronization
            auto now = std::chrono::steady_clock::now();
            uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            
            // Calculate which trace point we should be at based on elapsed time
            // This ensures all links use the same time points regardless of when they started
            uint64_t current_time_ms = (elapsed_ms / trace_interval) * trace_interval;
            
            // If we've reached or passed the end of the trace, wrap around
            if (current_time_ms > last_trace_time) {
                // Calculate how many complete passes through the file we've made
                uint64_t complete_passes = current_time_ms / (last_trace_time + trace_interval);
                uint64_t time_within_current_pass = current_time_ms % (last_trace_time + trace_interval);
                
                // If we're in the "gap" between the end of the file and the next multiple of trace_interval,
                // use the last valid time point
                if (time_within_current_pass > last_trace_time) {
                    current_time_ms = last_trace_time;
                } else {
                    current_time_ms = time_within_current_pass;
                }
                
                // Only log when we complete a full pass
                if (elapsed_ms % (last_trace_time + trace_interval) < trace_interval) {
                    LOG_INFO("[PHY Sender] Link " << traceColumnIndex << " reached end of trace file, restarting from beginning");
                }

                LOG_ERROR("[PHY Sender] Link " << traceColumnIndex << " reached end of trace file, terminating thread");

                // Terminate the thread if we reach the end of the trace file
                std::raise(SIGTERM);
            }
            
            // Find the exact matching trace time - binary search for efficiency
            size_t matching_index = 0;
            bool found_match = false;
            
            // Binary search for the matching time point
            size_t left = 0;
            size_t right = trace_times.size() - 1;
            
            while (left <= right) {
                size_t mid = left + (right - left) / 2;
                
                if (trace_times[mid] == current_time_ms) {
                    matching_index = mid;
                    found_match = true;
                    break;
                } else if (trace_times[mid] < current_time_ms) {
                    left = mid + 1;
                } else {
                    if (mid == 0) break; // Avoid underflow
                    right = mid - 1;
                }
            }
            
            // If no exact match, find the closest time point that's <= current_time_ms
            if (!found_match) {
                for (size_t i = 0; i < trace_times.size(); i++) {
                    if (trace_times[i] <= current_time_ms && 
                        (i == trace_times.size() - 1 || trace_times[i + 1] > current_time_ms)) {
                        matching_index = i;
                        found_match = true;
                        break;
                    }
                }
                
                // If still no match (shouldn't happen unless file is invalid), use the first entry
                if (!found_match) {
                    matching_index = 0;
                }
            }
            
            // Only update if we moved to a new entry or this is the first update
            if (matching_index != current_index || elapsed_ms < trace_interval) {
                // Get the corresponding bandwidth value
                double mbps;
                
                if (!idealModeEnabled.load()) {
                    // 단일 링크 모드
                    mbps = getBandwidthAtTime(trace_times[matching_index], traceColumnIndex);
                    // primaryBandwidth는 단일 링크 그대로 기록
                    primaryBandwidth.store(static_cast<uint32_t>(mbps * 1e6),
                                            std::memory_order_relaxed);
                } else {
                    // ideal: combined-bandwidth (5G + 4G) on a single virtual link
                    int otherColumn = (traceColumnIndex == 0 ? 1 : 0);
                    double v_primary   = getBandwidthAtTime(trace_times[matching_index], traceColumnIndex);
                    double v_secondary = getBandwidthAtTime(trace_times[matching_index], otherColumn);
                    mbps = v_primary + v_secondary;
                    primaryBandwidth.store(static_cast<uint32_t>(v_primary * 1e6),
                                            std::memory_order_relaxed);
                }
                // bps 변환 및 세팅
                uint32_t bps = static_cast<uint32_t>(mbps * 1e6);
                setBandwidth(bps);

                uint32_t curTotalBytes = bytesSent.load();
                uint32_t deltaTotal    = curTotalBytes - prevTotalBytes;
                prevTotalBytes         = curTotalBytes;

                if (csvLog.is_open()) {
                    if (!idealModeEnabled.load()) {
                        if (!headerWritten) {
                            csvLog << "time,usage\n";
                            headerWritten = true;
                        }
                        csvLog << current_time_ms << "," << deltaTotal << "\n";
                    } else {
                        // ideal: time,primary_usage,secondary_usage

                        // 1) compute how many "extra" bytes were sent this interval, safely
                        uint32_t curExtra = extraBytesSent.load(std::memory_order_relaxed);
                        uint32_t deltaExtra = 0;
                        if (curExtra >= prevExtraBytes) {
                            deltaExtra = curExtra - prevExtraBytes;
                        } else {
                            // counter wrapped or reset — treat as all new extra
                            deltaExtra = curExtra;
                        }
                        prevExtraBytes = curExtra;

                        // 2) clamp so we never subtract more than total
                        if (deltaExtra > deltaTotal) {
                            deltaExtra = deltaTotal;
                        }

                        // 3) primary usage is the remainder
                        uint32_t deltaPrimary = deltaTotal - deltaExtra;

                        // write header once
                        if (!headerWritten) {
                            csvLog << "time,primary_usage,secondary_usage\n";
                            headerWritten = true;
                        }

                        // log this slot
                        csvLog << current_time_ms << ","
                            << deltaPrimary << ","
                            << deltaExtra   << "\n";
                    }
                    csvLog.flush();
                }
                
                current_index = matching_index;
            }
            
            // Calculate exactly when the next update should happen based on our shared global time
            uint64_t next_time_point_ms = current_time_ms + trace_interval;
            auto now_after_update = std::chrono::steady_clock::now();
            uint64_t current_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_after_update - start_time).count();
            
            // Calculate how long to sleep until next update time
            int64_t sleep_time_ms = (next_time_point_ms - current_elapsed_ms);
            
            // Sleep until exactly the next update time, with safety bounds
            if (sleep_time_ms > 0 && sleep_time_ms <= trace_interval) {
                // Normal case: wait until the next update
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));
            } else if (sleep_time_ms <= 0) {
                // We're already past the next update time - sleep for a minimal amount
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                LOG_DEBUG("[PHY Sender] Link " << traceColumnIndex << " update loop running behind schedule");
            } else {
                // Safety fallback in case of calculation issues - sleep for half the interval
                std::this_thread::sleep_for(std::chrono::milliseconds(trace_interval / 2));
                LOG_DEBUG("[PHY Sender] Link " << traceColumnIndex << " using fallback sleep duration");
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR("[PHY Sender] Error in bandwidth update: " << e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(updateInterval));
        }
    }
}