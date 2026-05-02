#pragma once

#include "mocks/scheduler_types.hpp"
#include "RIC/zmq_interface.hpp"

namespace ric {

// Base CU controller interface for mocking
class CuSchedulerController {
public:
    virtual ~CuSchedulerController() = default;
    virtual bool initialize() = 0;
    virtual bool applySchedulingDecision(const SchedulingDecision& decision) = 0;
};

// Mock CU scheduler controller for testing
class MockCuSchedulerController : public CuSchedulerController {
public:
    MockCuSchedulerController() 
        : CuSchedulerController() {} // Default constructor for testing
    
    bool initialize() override {
        return true;
    }
    
    bool applySchedulingDecision(const SchedulingDecision& decision) override {
        last_decision_ = decision;
        return true;
    }
    
    SchedulingDecision getLastDecision() const {
        return last_decision_;
    }
    
private:
    SchedulingDecision last_decision_;
};

} // namespace ric