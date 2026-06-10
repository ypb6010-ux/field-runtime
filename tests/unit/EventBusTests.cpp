// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/bus/EventBus.h"

namespace {

struct Foo { int v = 0; };
struct Bar { std::string s; };

} // namespace

TEST_CASE("EventBus delivers events to a single subscriber", "[bus]") {
    core::bus::EventBus bus;
    int received = 0;
    auto sub = bus.subscribe<Foo>([&](Foo const& f) { received = f.v; });

    bus.publish(Foo{42});

    REQUIRE(received == 42);
}

TEST_CASE("EventBus does not deliver to unrelated types", "[bus]") {
    core::bus::EventBus bus;
    bool fooCalled = false;
    auto sub = bus.subscribe<Foo>([&](Foo const&) { fooCalled = true; });

    bus.publish(Bar{"hello"});

    REQUIRE_FALSE(fooCalled);
}

TEST_CASE("EventBus delivers to multiple subscribers in registration order",
          "[bus]") {
    core::bus::EventBus bus;
    std::vector<int> order;
    auto a = bus.subscribe<Foo>([&](Foo const& f) { order.push_back(f.v * 10 + 1); });
    auto b = bus.subscribe<Foo>([&](Foo const& f) { order.push_back(f.v * 10 + 2); });

    bus.publish(Foo{5});

    REQUIRE(order == std::vector<int>{51, 52});
}

TEST_CASE("Subscription destruction stops delivery", "[bus][subscription]") {
    core::bus::EventBus bus;
    int received = 0;
    {
        auto sub = bus.subscribe<Foo>([&](Foo const& f) { received = f.v; });
        bus.publish(Foo{1});
        REQUIRE(received == 1);
    }
    bus.publish(Foo{99});
    REQUIRE(received == 1);
}

TEST_CASE("Subscription cancel() stops delivery and updates active()",
          "[bus][subscription]") {
    core::bus::EventBus bus;
    int received = 0;
    auto sub = bus.subscribe<Foo>([&](Foo const& f) { received = f.v; });

    bus.publish(Foo{1});
    REQUIRE(sub.active());

    sub.cancel();
    REQUIRE_FALSE(sub.active());

    bus.publish(Foo{2});
    REQUIRE(received == 1);
}

TEST_CASE("Subscription is move-only and transfers ownership cleanly",
          "[bus][subscription]") {
    core::bus::EventBus bus;
    int received = 0;
    auto s1 = bus.subscribe<Foo>([&](Foo const& f) { received = f.v; });
    auto s2 = std::move(s1);

    REQUIRE_FALSE(s1.active());
    REQUIRE(s2.active());

    bus.publish(Foo{7});
    REQUIRE(received == 7);

    // Move-assignment also transfers and the previous destination's
    // subscription is dropped.
    int otherReceived = 0;
    auto s3 = bus.subscribe<Foo>([&](Foo const& f) { otherReceived = f.v; });
    s3 = std::move(s2);
    bus.publish(Foo{9});
    REQUIRE(received      == 9);   // s3 now points at the first handler
    REQUIRE(otherReceived == 0);   // the original s3 handler is gone
}

TEST_CASE("EventBus publish with no subscribers is a no-op", "[bus]") {
    core::bus::EventBus bus;
    REQUIRE_NOTHROW(bus.publish(Foo{1}));
    auto s = bus.stats();
    REQUIRE(s.totalPublished == 1);
    REQUIRE(s.totalDelivered == 0);
    REQUIRE(s.activeSubscribers == 0);
}

TEST_CASE("EventBus stats count publishes, deliveries, and live subscribers",
          "[bus][stats]") {
    core::bus::EventBus bus;
    auto a = bus.subscribe<Foo>([](Foo const&) {});
    auto b = bus.subscribe<Foo>([](Foo const&) {});

    bus.publish(Foo{0});
    bus.publish(Foo{0});

    auto s = bus.stats();
    REQUIRE(s.totalPublished    == 2);
    REQUIRE(s.totalDelivered    == 4);
    REQUIRE(s.activeSubscribers == 2);
}

TEST_CASE("Cancelled subscriptions are excluded from activeSubscribers",
          "[bus][stats]") {
    core::bus::EventBus bus;
    auto a = bus.subscribe<Foo>([](Foo const&) {});
    auto b = bus.subscribe<Foo>([](Foo const&) {});
    a.cancel();

    // Publish triggers lazy compaction of expired weak refs.
    bus.publish(Foo{0});

    REQUIRE(bus.stats().activeSubscribers == 1);
}

TEST_CASE("EventBus tolerates handler that cancels itself during dispatch",
          "[bus]") {
    core::bus::EventBus bus;
    std::optional<core::bus::Subscription> self;
    int calls = 0;
    self.emplace(bus.subscribe<Foo>([&](Foo const&) {
        ++calls;
        self.reset();           // drop self mid-dispatch
    }));

    REQUIRE_NOTHROW(bus.publish(Foo{0}));
    REQUIRE(calls == 1);

    bus.publish(Foo{0});
    REQUIRE(calls == 1);        // no further delivery after self-cancel
}

TEST_CASE("EventBus tolerates handler that subscribes during dispatch",
          "[bus]") {
    core::bus::EventBus bus;
    int outerCalls = 0;
    int innerCalls = 0;
    std::optional<core::bus::Subscription> innerSub;

    auto outer = bus.subscribe<Foo>([&](Foo const&) {
        ++outerCalls;
        if (!innerSub.has_value()) {
            innerSub.emplace(bus.subscribe<Foo>(
                [&](Foo const&) { ++innerCalls; }));
        }
    });

    bus.publish(Foo{0});
    REQUIRE(outerCalls == 1);
    REQUIRE(innerCalls == 0);   // inner subscribed after dispatch snapshot

    bus.publish(Foo{0});
    REQUIRE(outerCalls == 2);
    REQUIRE(innerCalls == 1);   // now visible
}

TEST_CASE("EventBus is safe under concurrent publish from many threads",
          "[bus][thread]") {
    core::bus::EventBus bus;
    std::atomic<int> count{0};
    auto sub = bus.subscribe<Foo>(
        [&](Foo const&) { count.fetch_add(1, std::memory_order_relaxed); });

    constexpr int kThreads   = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) bus.publish(Foo{i});
        });
    }
    for (auto& t : ts) t.join();

    REQUIRE(count.load() == kThreads * kPerThread);
    REQUIRE(bus.stats().totalPublished == quint64(kThreads * kPerThread));
    REQUIRE(bus.stats().totalDelivered == quint64(kThreads * kPerThread));
}

TEST_CASE("Subscriptions outlive other subscriptions independently",
          "[bus][subscription]") {
    core::bus::EventBus bus;
    int aCount = 0;
    int bCount = 0;
    auto a = bus.subscribe<Foo>([&](Foo const&) { ++aCount; });
    {
        auto b = bus.subscribe<Foo>([&](Foo const&) { ++bCount; });
        bus.publish(Foo{0});
    }
    bus.publish(Foo{0});
    REQUIRE(aCount == 2);
    REQUIRE(bCount == 1);
}
