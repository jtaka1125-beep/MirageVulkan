// =============================================================================
// Unit tests for EventBus (src/event_bus.hpp)
// =============================================================================
#include <gtest/gtest.h>
#include "event_bus.hpp"

using namespace mirage;

// ---------------------------------------------------------------------------
// Basic subscribe + publish
// ---------------------------------------------------------------------------
TEST(EventBusTest, SubscribeAndPublish) {
    EventBus bus;
    int received_count = 0;
    std::string received_id;

    auto sub = bus.subscribe<DeviceConnectedEvent>(
        [&](const DeviceConnectedEvent& e) {
            received_count++;
            received_id = e.device_id;
        });

    DeviceConnectedEvent ev;
    ev.device_id = "device-1";
    ev.display_name = "Test Device";
    ev.connection_type = "usb";
    bus.publish(ev);

    EXPECT_EQ(received_count, 1);
    EXPECT_EQ(received_id, "device-1");
}

// ---------------------------------------------------------------------------
// Multiple subscribers for the same event
// ---------------------------------------------------------------------------
TEST(EventBusTest, MultipleSubscribers) {
    EventBus bus;
    int count_a = 0;
    int count_b = 0;

    auto sub_a = bus.subscribe<ShutdownEvent>(
        [&](const ShutdownEvent&) { count_a++; });
    auto sub_b = bus.subscribe<ShutdownEvent>(
        [&](const ShutdownEvent&) { count_b++; });

    bus.publish(ShutdownEvent{});

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);
}

// ---------------------------------------------------------------------------
// Unsubscribe via RAII handle destruction
// ---------------------------------------------------------------------------
TEST(EventBusTest, UnsubscribeOnHandleDestruction) {
    EventBus bus;
    int count = 0;

    {
        auto sub = bus.subscribe<ShutdownEvent>(
            [&](const ShutdownEvent&) { count++; });
        bus.publish(ShutdownEvent{});
        EXPECT_EQ(count, 1);
        // sub goes out of scope here -> unsubscribed
    }

    bus.publish(ShutdownEvent{});
    EXPECT_EQ(count, 1); // should NOT increase
}

// ---------------------------------------------------------------------------
// has_subscribers reflects current state
// ---------------------------------------------------------------------------
TEST(EventBusTest, HasSubscribers) {
    EventBus bus;
    EXPECT_FALSE(bus.has_subscribers<ShutdownEvent>());

    {
        auto sub = bus.subscribe<ShutdownEvent>(
            [](const ShutdownEvent&) {});
        EXPECT_TRUE(bus.has_subscribers<ShutdownEvent>());
    }

    EXPECT_FALSE(bus.has_subscribers<ShutdownEvent>());
}

// ---------------------------------------------------------------------------
// Different event types are independent
// ---------------------------------------------------------------------------
TEST(EventBusTest, EventTypeIsolation) {
    EventBus bus;
    int connect_count = 0;
    int disconnect_count = 0;

    auto sub1 = bus.subscribe<DeviceConnectedEvent>(
        [&](const DeviceConnectedEvent&) { connect_count++; });
    auto sub2 = bus.subscribe<DeviceDisconnectedEvent>(
        [&](const DeviceDisconnectedEvent&) { disconnect_count++; });

    DeviceConnectedEvent ce;
    ce.device_id = "d1";
    bus.publish(ce);

    EXPECT_EQ(connect_count, 1);
    EXPECT_EQ(disconnect_count, 0);
}

// ---------------------------------------------------------------------------
// release() keeps subscription alive after handle destruction
// ---------------------------------------------------------------------------
TEST(EventBusTest, ReleaseKeepsSubscription) {
    EventBus bus;
    int count = 0;

    {
        auto sub = bus.subscribe<ShutdownEvent>(
            [&](const ShutdownEvent&) { count++; });
        sub.release(); // detach from RAII
    }

    bus.publish(ShutdownEvent{});
    EXPECT_EQ(count, 1); // still subscribed
}

// ---------------------------------------------------------------------------
// Handler exception does not crash bus or prevent other handlers
// ---------------------------------------------------------------------------
TEST(EventBusTest, HandlerExceptionIsCaught) {
    EventBus bus;
    int good_count = 0;

    auto sub1 = bus.subscribe<ShutdownEvent>(
        [](const ShutdownEvent&) { throw std::runtime_error("boom"); });
    auto sub2 = bus.subscribe<ShutdownEvent>(
        [&](const ShutdownEvent&) { good_count++; });

    EXPECT_NO_THROW(bus.publish(ShutdownEvent{}));
    EXPECT_EQ(good_count, 1);
}

// ---------------------------------------------------------------------------
// Move semantics for SubscriptionHandle
// ---------------------------------------------------------------------------
TEST(EventBusTest, HandleMoveSemantic) {
    EventBus bus;
    int count = 0;

    SubscriptionHandle outer;
    {
        auto inner = bus.subscribe<ShutdownEvent>(
            [&](const ShutdownEvent&) { count++; });
        outer = std::move(inner);
        // inner is moved-from, should not unsubscribe
    }

    bus.publish(ShutdownEvent{});
    EXPECT_EQ(count, 1); // still subscribed via outer
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
