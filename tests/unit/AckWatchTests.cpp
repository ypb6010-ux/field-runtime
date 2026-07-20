// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
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
        bus.publish(bus::DpChanged{"belt2.fb.contactor", true, QDateTime{}});
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
        bus.publish(bus::DpChanged{"other.dp", 42, QDateTime{}});
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
        bus.publish(bus::DpChanged{"dp.x", 0, QDateTime{}});
        std::this_thread::sleep_for(10ms);
        bus.publish(bus::DpChanged{"dp.x", 2, QDateTime{}});
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

TEST_CASE("AckWatch observes an acknowledgement that arrived before waitOnce",
          "[ack][stability][race]") {
    bus::EventBus bus;
    dp::DatapointRegistry datapoints;
    dp::DatapointSpec spec;
    spec.id = QStringLiteral("dp.fast");
    auto point = std::make_shared<dp::Datapoint>(spec);
    datapoints.registerDp(point);
    point->setValue(true);

    module::AckWatch watch(cfgFor("dp.fast", true, 3000), bus, &datapoints);
    watch.start();
    auto const t0 = std::chrono::steady_clock::now();
    REQUIRE(watch.waitOnce() == module::AckWatch::AckResult::Ok);
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(elapsed < 100);
}

TEST_CASE("AckWatch cancel wakes an active waiter immediately",
          "[ack][cancel][stability]") {
    bus::EventBus bus;
    module::AckWatch watch(cfgFor("dp.silent", true, 3000), bus);
    watch.start();

    auto waiting = std::async(std::launch::async, [&] { return watch.waitOnce(); });
    std::this_thread::sleep_for(20ms);

    auto const t0 = std::chrono::steady_clock::now();
    watch.cancel();
    REQUIRE(waiting.get() == module::AckWatch::AckResult::Cancelled);
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(elapsed < 500);
}
