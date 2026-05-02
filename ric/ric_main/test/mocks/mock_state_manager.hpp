#pragma once

#include "state_manager/state_manager.hpp"
#include "mocks/link_state.hpp"
#include <unordered_map>
#include <mutex>
#include <string>

namespace ric {

// Mock state manager for testing
class MockStateManager : public StateManager {
public:
    MockStateManager() : StateManager() {}
    ~MockStateManager() = default;

    bool initialize() override {
        return true;
    }

    void updatePdcpStatus(const std::string& user_id, const std::string& metrics) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        pdcp_metrics_[user_id] = metrics;
    }

    void updateRlcStatus(const std::string& user_id, const std::string& metrics) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        rlc_metrics_[user_id] = metrics;
    }

    std::string getPdcpStatus(const std::string& user_id) const {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        if (pdcp_metrics_.find(user_id) != pdcp_metrics_.end()) {
            return pdcp_metrics_.at(user_id);
        }
        return "";
    }

    std::string getRlcStatus(const std::string& user_id) const {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        if (rlc_metrics_.find(user_id) != rlc_metrics_.end()) {
            return rlc_metrics_.at(user_id);
        }
        return "";
    }

private:
    mutable std::mutex metrics_mutex_;
    std::unordered_map<std::string, std::string> pdcp_metrics_;
    std::unordered_map<std::string, std::string> rlc_metrics_;
};

} // namespace ric