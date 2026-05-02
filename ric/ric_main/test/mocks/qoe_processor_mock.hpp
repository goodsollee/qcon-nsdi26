#pragma once

#include <gmock/gmock.h>
#include "RIC/qoe/qoe_processor.hpp"

namespace ric {

class MockQoEProcessor : public QoEProcessor {
public:
    MOCK_METHOD(bool, initialize, (), (override));
    MOCK_METHOD(void, setFrameCompletionCallback, 
                (std::function<void(uint32_t, std::chrono::milliseconds)> callback), 
                (override));
    MOCK_METHOD(void, setNewFrameCallback, (std::function<void()> callback), (override));
    MOCK_METHOD(Json::Value, getQoEMetrics, (), (const, override));
    MOCK_METHOD(Json::Value, getJitterStats, (), (const, override));
    MOCK_METHOD(void, processFrame, (uint32_t timestamp, double size_kb), (override));
    MOCK_METHOD(void, updateFrameProgress, 
                (uint32_t timestamp, double progress, int link_id), 
                (override));
    MOCK_METHOD(std::map<uint32_t, FrameStatus>, getActiveFrames, (), (const, override));
};
} 