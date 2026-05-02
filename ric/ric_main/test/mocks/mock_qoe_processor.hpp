#pragma once

#include "RIC/qoe_processor.hpp"
#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <atomic>
#include <unordered_map>

namespace ric {

// Mock QoE processor for testing
class MockQoEProcessor : public QoEProcessor {
public:
    MockQoEProcessor() : QoEProcessor() {}
    ~MockQoEProcessor() {
        stop();
    }

    bool initialize(const std::string& interface, bool test_mode = false) override {
        interface_name_ = interface;
        test_mode_ = test_mode;
        return true;
    }
    
    bool start() override {
        running_ = true;
        processing_thread_ = std::thread(&MockQoEProcessor::processingThread, this);
        return true;
    }
    
    void stop() override {
        running_ = false;
        if (processing_thread_.joinable()) {
            processing_thread_.join();
        }
    }
    
    void addRtpPackets(const std::vector<PacketInfo>& packets) {
        std::lock_guard<std::mutex> lock(packets_mutex_);
        for (const auto& packet : packets) {
            packet_queue_.push_back(packet);
        }
    }
    
    // Override QoEProcessor methods
    std::unordered_map<uint32_t, FrameInfo> getActiveFramesForUser(const std::string& user_id) const override {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        
        if (user_frames_.find(user_id) != user_frames_.end()) {
            return user_frames_.at(user_id);
        }
        
        return {};
    }
    
    std::vector<std::string> getActiveUsers() const override {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        std::vector<std::string> users;
        
        for (const auto& [user_id, frames] : user_frames_) {
            users.push_back(user_id);
        }
        
        return users;
    }
    
private:
    std::atomic<bool> running_{false};
    std::thread processing_thread_;
    std::vector<PacketInfo> packet_queue_;
    std::mutex packets_mutex_;
    
    // User frame tracking (similar to actual QoEProcessor)
    mutable std::mutex frames_mutex_;
    std::unordered_map<std::string, std::unordered_map<uint32_t, FrameInfo>> user_frames_;
    
    std::string interface_name_;
    bool test_mode_;
    
    void processingThread() {
        while (running_) {
            processPackets();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    void processPackets() {
        std::vector<PacketInfo> packets;
        
        {
            std::lock_guard<std::mutex> lock(packets_mutex_);
            packets.swap(packet_queue_);
        }
        
        for (const auto& packet : packets) {
            processRtpPacket(packet);
        }
    }
    
    void processRtpPacket(const PacketInfo& info) {
        if (!info.is_rtp || info.payload_type != VIDEO_PAYLOAD_TYPE) {
            return;
        }
        
        // Convert IP to string format for user ID
        std::stringstream ss;
        ss << ((info.dst_ip >> 24) & 0xFF) << "."
           << ((info.dst_ip >> 16) & 0xFF) << "."
           << ((info.dst_ip >> 8) & 0xFF) << "."
           << (info.dst_ip & 0xFF);
        std::string user_id = ss.str();
        
        // Create or update frame info
        std::lock_guard<std::mutex> lock(frames_mutex_);
        auto& user_frame_map = user_frames_[user_id];
        
        if (user_frame_map.find(info.timestamp) == user_frame_map.end()) {
            FrameInfo frame;
            frame.timestamp = info.timestamp;
            frame.start_time = std::chrono::steady_clock::now();
            frame.last_packet_time = frame.start_time;
            frame.total_size = 0;
            frame.received_size = 0;
            frame.required_size = 0; // Will be updated when marker arrives
            frame.status = FrameDeliveryStatus::IN_PROGRESS;
            frame.frame_delivery_progress = 0.0;
            frame.has_marker = false;
            frame.dst_ip = info.dst_ip;
            frame.user_id = user_id;
            
            user_frame_map[info.timestamp] = frame;
        }
        
        auto& frame = user_frame_map[info.timestamp];
        frame.total_size += info.packet_size;
        frame.received_size += info.packet_size;
        frame.last_packet_time = std::chrono::steady_clock::now();
        
        // Update progress
        if (frame.total_size > 0) {
            frame.frame_delivery_progress = static_cast<double>(frame.received_size) / frame.total_size;
        }
        
        // If marker packet, mark the frame as complete
        if (info.marker) {
            frame.has_marker = true;
            frame.marker_arrival_time = frame.last_packet_time;
            frame.required_size = frame.total_size;
            
            // Update status if fully received
            if (frame.received_size >= frame.required_size) {
                frame.status = FrameDeliveryStatus::COMPLETE;
                frame.delivery_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame.last_packet_time - frame.start_time
                );
            }
        }
    }
};

} // namespace ric