#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "scheduler/QCON/qcon_scheduler.hpp"
#include "RIC/state/state_manager_mock.hpp"
#include "RIC/controller/cu_scheduler_controller_mock.hpp"
#include "RIC/qoe/qoe_processor_mock.hpp"

using namespace ric;
using namespace testing;

class QconSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_manager_ = std::make_shared<MockStateManager>();
        controller_ = std::make_shared<MockCuSchedulerController>();
        qoe_processor_ = std::make_shared<MockQoEProcessor>();
        
        scheduler_ = std::make_shared<QconScheduler>();
        
        // Setup default configuration
        SchedulerConfig config;
        config.scheduler_name = "QCON";
        config.hysteresis_margin = 0.1;
        config.min_switch_interval_ms = 100;
        
        EXPECT_TRUE(scheduler_->initialize(state_manager_, config));
        scheduler_->setController(controller_);
        scheduler_->setQoEProcessor(qoe_processor_);
    }

    std::shared_ptr<QconScheduler> scheduler_;
    std::shared_ptr<MockStateManager> state_manager_;
    std::shared_ptr<MockCuSchedulerController> controller_;
    std::shared_ptr<MockQoEProcessor> qoe_processor_;
};

// Test initialization
TEST_F(QconSchedulerTest, InitializationTest) {
    EXPECT_EQ(scheduler_->getConfig().scheduler_name, "QCON");
    EXPECT_EQ(scheduler_->getCurrentLinkId(), 0);
    
    auto stats = scheduler_->getExtendedStatistics();
    EXPECT_EQ(stats["primary_link_selections"].asUInt64(), 0);
    EXPECT_EQ(stats["backup_link_selections"].asUInt64(), 0);
    EXPECT_EQ(stats["forwarding_events"].asUInt64(), 0);
}

// Test link selection
TEST_F(QconSchedulerTest, LinkSelectionTest) {
    // Setup link metrics
    LinkState link1_state;
    link1_state.metrics.throughput_mbps = 100.0;
    link1_state.metrics.delay_ms = 10.0;
    link1_state.metrics.packet_loss = 0.01;
    
    LinkState link2_state;
    link2_state.metrics.throughput_mbps = 50.0;
    link2_state.metrics.delay_ms = 20.0;
    link2_state.metrics.packet_loss = 0.02;
    
    // Update link states
    scheduler_->onLinkStateUpdated(1, link1_state);
    scheduler_->onLinkStateUpdated(2, link2_state);
    
    // Simulate frame arrival
    uint32_t frame_timestamp = 1000;
    double frame_size_kb = 1000.0;
    
    // Expect the scheduler to select link 1 (better metrics)
    EXPECT_CALL(*controller_, sendCommandImmediate(1))
        .WillOnce(Return(true));
    
    scheduler_->handleNewFrame();
    auto decision = scheduler_->makeDecision();
    EXPECT_EQ(decision.selected_link_id, 1);
}

// Test packet forwarding
TEST_F(QconSchedulerTest, PacketForwardingTest) {
    // Enable packet forwarding
    QconSchedulerConfig qcon_config;
    qcon_config.enable_packet_forwarding = true;
    scheduler_->updateQconConfig(qcon_config);
    
    // Setup congested link state
    LinkState link_state;
    link_state.metrics.throughput_mbps = 10.0;
    link_state.metrics.delay_ms = 100.0;
    link_state.metrics.buffer_occupancy_ratio = 0.9;
    link_state.metrics.is_congested = true;
    
    scheduler_->onLinkStateUpdated(1, link_state);
    
    // Check if packet forwarding is triggered
    EXPECT_TRUE(scheduler_->shouldForwardPackets(1));
}

// Test frame deadline calculation
TEST_F(QconSchedulerTest, DeadlineCalculationTest) {
    uint32_t frame_timestamp = 1000;
    double remaining_bytes_kb = 500.0;
    double last_frame_completion_time_ms = 33.0;
    
    auto deadline = scheduler_->calculateFrameDeadline(
        frame_timestamp, 
        remaining_bytes_kb,
        last_frame_completion_time_ms
    );
    
    // Deadline should be positive and reasonable
    EXPECT_GT(deadline, 0.0);
    EXPECT_LT(deadline, 1000.0);  // Arbitrary upper bound for test
}

// Test QoE metrics handling
TEST_F(QconSchedulerTest, QoEMetricsTest) {
    // Setup mock QoE metrics
    Json::Value mock_qoe_metrics;
    mock_qoe_metrics["vmaf_score"] = 90.0;
    mock_qoe_metrics["frame_delay"] = 20.0;
    
    EXPECT_CALL(*qoe_processor_, getQoEMetrics())
        .WillOnce(Return(mock_qoe_metrics));
    
    auto stats = scheduler_->getExtendedStatistics();
    EXPECT_TRUE(stats.isMember("qoe_metrics"));
    EXPECT_EQ(stats["qoe_metrics"]["vmaf_score"].asDouble(), 90.0);
}

// Test frame completion handling
TEST_F(QconSchedulerTest, FrameCompletionTest) {
    uint32_t frame_timestamp = 1000;
    std::chrono::milliseconds completion_time(33);
    
    // Setup expectations for logging
    scheduler_->handleFrameCompletion(frame_timestamp, completion_time);
    
    auto stats = scheduler_->getExtendedStatistics();
    EXPECT_GT(stats["avg_frame_completion_time"].asDouble(), 0.0);
}

// Test chunk size computation
TEST_F(QconSchedulerTest, ChunkSizeComputationTest) {
    double total_size_kb = 1000.0;
    std::vector<int> link_ids = {1, 2};
    
    // Setup link metrics
    LinkState link1_state;
    link1_state.metrics.throughput_mbps = 100.0;
    
    LinkState link2_state;
    link2_state.metrics.throughput_mbps = 50.0;
    
    scheduler_->onLinkStateUpdated(1, link1_state);
    scheduler_->onLinkStateUpdated(2, link2_state);
    
    auto chunk_sizes = scheduler_->computeChunkSizes(total_size_kb, link_ids);
    
    EXPECT_EQ(chunk_sizes.size(), 2);
    EXPECT_GT(chunk_sizes[0], chunk_sizes[1]); // First link should get larger chunk
    EXPECT_NEAR(chunk_sizes[0] + chunk_sizes[1], total_size_kb, 0.1);
}

// Test configuration updates
TEST_F(QconSchedulerTest, ConfigurationUpdateTest) {
    QconSchedulerConfig new_config;
    new_config.enable_packet_forwarding = true;
    new_config.congestion_threshold = 0.8;
    
    scheduler_->updateQconConfig(new_config);
    
    auto config = scheduler_->getQconConfig();
    EXPECT_EQ(config.enable_packet_forwarding, true);
    EXPECT_NEAR(config.congestion_threshold, 0.8, 0.001);
} 