#pragma once

#include <gmock/gmock.h>
#include "RIC/controller/cu_scheduler_controller.hpp"

namespace ric {

class MockCuSchedulerController : public CuSchedulerController {
public:
    MOCK_METHOD(bool, initialize, (), (override));
    MOCK_METHOD(bool, sendCommand, (int link_id), (override));
    MOCK_METHOD(bool, sendCommandImmediate, (int link_id), (override));
    MOCK_METHOD(void, setCommandCallback, (std::function<void(int)> callback), (override));
    MOCK_METHOD(int, getCurrentLinkId, (), (const, override));
};
} 