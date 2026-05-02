#pragma once

#include <gmock/gmock.h>
#include "RIC/state/state_manager.hpp"

namespace ric {

class MockStateManager : public StateManager {
public:
    MOCK_METHOD(bool, initialize, (), (override));
    MOCK_METHOD(void, registerListener, (StateListener* listener), (override));
    MOCK_METHOD(void, unregisterListener, (StateListener* listener), (override));
    MOCK_METHOD(void, updateLinkState, (int link_id, const LinkState& state), (override));
    MOCK_METHOD(void, updateSystemState, (const Json::Value& state), (override));
    MOCK_METHOD(LinkState, getLinkState, (int link_id), (const, override));
    MOCK_METHOD(std::map<int, LinkState>, getAllLinkStates, (), (const, override));
    MOCK_METHOD(Json::Value, getSystemState, (), (const, override));
};
} 