// phy_layer.cpp
#include "phy_layer.h"
#include <iostream>

PHYLayer::PHYLayer(NetworkEmulator* emulator)
    : network_emulator_(emulator)
    , running_(false) {
}

PHYLayer::~PHYLayer() {
    stop();
}

bool PHYLayer::init() {
    if (!network_emulator_) {
        std::cerr << "PHY: Network emulator not initialized" << std::endl;
        return false;
    }
    return true;
}

void PHYLayer::start() {
    if (running_) {
        return;
    }

    running_ = true;
    worker_threads_.emplace_back(&PHYLayer::transmission_process, this);
    std::cout << "PHY: Started" << std::endl;
}

void PHYLayer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    tx_cv_.notify_all();

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
    std::cout << "PHY: Stopped" << std::endl;
}

void PHYLayer::receive_from_mac(const MACFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        tx_queue_.push(frame);
    }
    tx_cv_.notify_one();
}

void PHYLayer::transmission_process() {
    while (running_) {
        MACFrame frame;
        {
            std::unique_lock<std::mutex> lock(tx_mutex_);
            tx_cv_.wait(lock, [this] { 
                return !tx_queue_.empty() || !running_; 
            });

            if (!running_) {
                break;
            }

            if (!tx_queue_.empty()) {
                frame = std::move(tx_queue_.front());
                tx_queue_.pop();
            }
        }

        // Forward the data to network emulator
        // Network emulator will handle the bandwidth and latency emulation
        if (!frame.data.empty()) {
            network_emulator_->SendData(frame.data);
            
            // Log transmission for debugging
            std::cout << "PHY: Transmitted frame with HARQ ID " 
                      << frame.harq_id << ", size " 
                      << frame.data.size() << " bytes" << std::endl;
        }
    }
}

PHYLayer::NetworkConditions PHYLayer::get_network_conditions() const {
    NetworkConditions conditions;
    
    // Get current network conditions from emulator
    auto profile = network_emulator_->GetCurrentProfile();
    conditions.current_bandwidth_kbps = profile.bandwidth_kbps;
    conditions.current_latency_ms = profile.latency_ms;
    
    return conditions;
}