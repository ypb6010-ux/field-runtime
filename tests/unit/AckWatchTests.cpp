// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/module/AckWatch.h"

using namespace core;
using namespace std::chrono_literals;

namespace {

module::AckWatch::Config cfgFor(QString const& dpId,
                                 QVariant       expected,
                                 int            timeoutMs = 200) {
    module::AckWatch::Config c;
    c.moduleId  = "ack." + dpId;
    c.dpId      = dpId;
    c.expected  = std::move(expected);
    c.timeoutMs = timeoutMs;
    return c;
}

} // namespace

TEST_CASE("AckWatch resolves on the first matching DpChanged event",
          "[ack][ok]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("belt2.fb.contactor", true), bus);
    watch.start();

    std::thread publisher([&] {
        std::this_thread::sleep_for(20ms);
        bus.publish(bus::DpChanged{"belt2.fb.contactor", true, {}});
    });

    auto r = watch.waitOnce();
    publisher.join();
    REQUIRE(r == module::AckWatch::AckResult::Ok);
}

TEST_CASE("AckWatch ignores DpChanged events for other dps",
          "[ack][filter]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("dp.expected", 42, /*timeoutMs*/100), bus);
    watch.start();

    std::thread publisher([&] {
        std::this_thread::sleep_for(20ms);
        bus.publish(bus::DpChanged{"other.dp", std::int64_t(42), {}});
    });

    auto r = watch.waitOnce();
    publisher.join();
    REQUIRE(r == module::AckWatch::AckResult::Timeout);   // never matched
}

TEST_CASE("AckWatch ignores DpChanged events with the wrong value",
          "[ack][filter]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("dp.x", 1, 100), bus);
    watch.start();

    std::thread publisher([&] {
        std::this_thread::sleep_for(20ms);
        bus.publish(bus::DpChanged{"dp.x", std::int64_t(0), {}});
        std::this_thread::sleep_for(10ms);
        bus.publish(bus::DpChanged{"dp.x", std::int64_t(2), {}});
    });

    auto r = watch.waitOnce();
    publisher.join();
    REQUIRE(r == module::AckWatch::AckResult::Timeout);
}

TEST_CASE("AckWatch times out when no matching event arrives",
          "[ack][timeout]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("dp.silent", true, 60), bus);
    watch.start();

    auto t0 = std::chrono::steady_clock::now();
    auto r = watch.waitOnce();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(r == module::AckWatch::AckResult::Timeout);
    REQUIRE(elapsed >= 50);
    REQUIRE(elapsed < 500);
}

TEST_CASE("AckWatch exposes its watched dp id", "[ack][meta]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("belt.fb", true), bus);
    REQUIRE(watch.dpId() == "belt.fb");
    REQUIRE(watch.id()   == "ack.belt.fb");
}
