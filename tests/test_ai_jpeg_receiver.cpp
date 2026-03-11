// =============================================================================
// AiJpegReceiver Unit Tests
// =============================================================================

#include <gtest/gtest.h>
#include "ai/ai_jpeg_receiver.hpp"
#include <thread>
#include <chrono>

using namespace mirage::ai;

class AiJpegReceiverTest : public ::testing::Test {
protected:
    void SetUp() override {
        receiver = std::make_unique<AiJpegReceiver>();
    }
    void TearDown() override {
        if (receiver && receiver->isRunning()) {
            receiver->stop();
        }
    }
    std::unique_ptr<AiJpegReceiver> receiver;
};

TEST_F(AiJpegReceiverTest, InitialState) {
    EXPECT_FALSE(receiver->isRunning());
    EXPECT_EQ(receiver->framesReceived(), 0u);
    EXPECT_EQ(receiver->bytesReceived(), 0u);
}

TEST_F(AiJpegReceiverTest, StartStop) {
    // Start on available port
    bool started = receiver->start("test_device", 51299);
    EXPECT_TRUE(started);
    EXPECT_TRUE(receiver->isRunning());

    // Stop
    receiver->stop();
    EXPECT_FALSE(receiver->isRunning());
}

TEST_F(AiJpegReceiverTest, DoubleStartFails) {
    EXPECT_TRUE(receiver->start("test_device", 51298));
    EXPECT_FALSE(receiver->start("test_device", 51298));  // Already running
    receiver->stop();
}

TEST_F(AiJpegReceiverTest, CallbackIsSet) {
    bool callback_called = false;
    receiver->setFrameCallback([&](const std::string&, const std::vector<uint8_t>&,
                                   int, int, int64_t) {
        callback_called = true;
    });
    // Callback won't be called without actual data, just verify it compiles
    EXPECT_FALSE(callback_called);
}

TEST_F(AiJpegReceiverTest, StatsReset) {
    // Start receiver
    EXPECT_TRUE(receiver->start("test", 51297));

    // Initial stats should be zero
    EXPECT_EQ(receiver->framesReceived(), 0u);
    EXPECT_EQ(receiver->bytesReceived(), 0u);

    // Stop and verify still zero (no data received)
    receiver->stop();
    EXPECT_EQ(receiver->framesReceived(), 0u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
