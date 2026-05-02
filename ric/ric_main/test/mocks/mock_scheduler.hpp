#pragma once

#include "mocks/scheduler_interface.hpp"
#include "RIC/qoe_processor.hpp"

namespace ric {

// Basic QCON scheduler interface for mocking
class QconScheduler : public BaseScheduler {
public:
    virtual ~QconScheduler() = default;
    virtual void setQoEProcessor(std::shared_ptr<QoEProcessor> qoe_processor) {}
};

// Mock QCON scheduler for testing
class MockQconScheduler : public QconScheduler {
public:
    MockQconScheduler() : QconScheduler() {}
    ~MockQconScheduler() = default;

    bool initialize(std::shared_ptr<StateManager> state_manager, 
                  const SchedulerConfig& config) override {
        state_manager_ = state_manager;
        config_ = config;
        return true;
    }

    bool start() override {
        running_ = true;
        return true;
    }

    void stop() override {
        running_ = false;
    }

    SchedulingDecision makeDecision() override {
        SchedulingDecision decision;
        decision.decision_type = "mock";
        decision.parameters["algorithm"] = "mock";
        return decision;
    }

private:
    bool running_ = false;
    SchedulerConfig config_;
    std::shared_ptr<StateManager> state_manager_;
};

} // namespace ric