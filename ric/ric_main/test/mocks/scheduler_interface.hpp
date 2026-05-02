#pragma once

#include "mocks/scheduler_types.hpp"
#include "state_manager/state_manager.hpp"
#include <memory>

namespace ric {

// Base scheduler interface
class SchedulerInterface {
public:
    virtual ~SchedulerInterface() = default;
    
    virtual bool initialize(std::shared_ptr<StateManager> state_manager, 
                         const SchedulerConfig& config) = 0;
    
    virtual bool start() = 0;
    
    virtual void stop() = 0;
    
    virtual SchedulingDecision makeDecision() = 0;
    
    virtual void setController(void* controller) {}
};

// Basic implementation for testing
class BaseScheduler : public SchedulerInterface, public StateUpdateListener {
public:
    virtual ~BaseScheduler() = default;
    
    void onLinkStateUpdated(int link_id, const LinkState& link_state) override {}
    
    void onSystemStateUpdated(const Json::Value& system_state) override {}
    
    bool initialize(std::shared_ptr<StateManager> state_manager, 
                  const SchedulerConfig& config) override {
        return true;
    }
    
    bool start() override {
        return true;
    }
    
    void stop() override {}
    
    SchedulingDecision makeDecision() override {
        return makeDecisionInternal();
    }
    
    // Internal method to be implemented by subclasses
    virtual SchedulingDecision makeDecisionInternal() {
        SchedulingDecision decision;
        decision.decision_type = "base";
        return decision;
    }
    
    virtual void updateLinkMetrics(const std::map<int, void*>& metrics) {}
};

// Factory to create schedulers
class SchedulerFactory {
public:
    static std::shared_ptr<SchedulerInterface> createScheduler(
        const std::string& scheduler_name,
        std::shared_ptr<StateManager> state_manager,
        const SchedulerConfig& config,
        void* controller = nullptr) {
        
        // In a mock, we just return nullptr
        return nullptr;
    }
};

} // namespace ric