// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/control/ControlArbiter.h"
#include "core/control/DeviceRouteManager.h"

using namespace core::control;

TEST_CASE("control addresses detect register and MQTT JSON conflicts",
          "[control]") {
    ControlAddress hr{"modbus", "10.0.0.2:502/1", "HR", {}, 10, 2};
    ControlAddress overlap{"modbus", "10.0.0.2:502/1", "HR", {}, 11, 1};
    ControlAddress otherUnit{"modbus", "10.0.0.2:502/2", "HR", {}, 11, 1};
    REQUIRE(hr.conflictsWith(overlap));
    REQUIRE_FALSE(hr.conflictsWith(otherUnit));

    ControlAddress mqtt{"mqtt", "broker-a", "device/cmd", "/motor", 0, 1};
    ControlAddress child{"mqtt", "broker-a", "device/cmd", "/motor/speed", 0, 1};
    ControlAddress sibling{"mqtt", "broker-a", "device/cmd", "/light", 0, 1};
    REQUIRE(mqtt.conflictsWith(child));
    REQUIRE_FALSE(mqtt.conflictsWith(sibling));
}

TEST_CASE("priority lease allows only owner or higher priority actor",
          "[control]") {
    ControlArbiter arbiter;
    arbiter.setPolicy("motor.speed", {"motor", PolicyMode::PriorityLease, 1000, 1});
    ControlTarget target{"motor.speed", "motor", "modbus-main", {}};

    auto first = arbiter.authorize(
        {"r1", {"operator", {}, {}, {}, "modbus", {}, 10}, target}, 100);
    REQUIRE(first.allowed);

    auto denied = arbiter.authorize(
        {"r2", {"web", {}, {}, {}, "web", {}, 5}, target}, 200);
    REQUIRE_FALSE(denied.allowed);
    REQUIRE(denied.ownerId == "operator");

    auto preempted = arbiter.authorize(
        {"r3", {"mu", {}, {}, {}, "modbus", {}, 20}, target}, 300);
    REQUIRE(preempted.allowed);
    REQUIRE(preempted.preempted);
    REQUIRE(preempted.ownerId == "mu");
}

TEST_CASE("device route manager keeps one active writable route per device",
          "[control]") {
    DeviceRouteManager routes;
    std::string error;
    REQUIRE(routes.configure({
        {"sdk", "vendor-device", "vendor-sdk", {}, "vendor", true, true},
        {"modbus", "vendor-device", {}, "plc", "modbus", true, false},
    }, error));
    REQUIRE(routes.isActive("vendor-device", "sdk"));
    REQUIRE(routes.setActive("vendor-device", "modbus", error));
    REQUIRE(routes.isActive("vendor-device", "modbus"));
    REQUIRE_FALSE(routes.isActive("vendor-device", "sdk"));
}

TEST_CASE("leases conflict across different target ids on the same address",
          "[control]") {
    ControlArbiter arbiter;
    arbiter.setDefaultPolicy(
        {"lease", PolicyMode::ExclusiveLease, 1000, 0});
    ControlTarget first{"speed-a", "motor", "main",
                        {"modbus", "10.0.0.2:502/1", "HR", {}, 10, 1}};
    ControlTarget alias{"speed-b", "motor", "main",
                        {"modbus", "10.0.0.2:502/1", "HR", {}, 10, 1}};
    REQUIRE(arbiter.authorize(
        {"1", {"box", {}, {}, {}, "modbus", {}, 1}, first}, 10).allowed);
    arbiter.setPolicy("speed-b", {"open-alias", PolicyMode::Open, 0, 0});
    REQUIRE_FALSE(arbiter.authorize(
        {"2", {"web", {}, {}, {}, "web", {}, 1}, alias}, 20).allowed);
}
