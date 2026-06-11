// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "core/module/Heartbeat.h"
#include "core/sched/SchedulerTypes.h"
#include "mocks/MockTransport.h"

using namespace core;
using namespace std::chrono_literals;

namespace {

module::Heartbeat::Config cfgFor(QString const& id = "hb",
                                  int            periodMs = 10,
                                  QList<quint16> values = {0xA5A5}) {
    module::Heartbeat::Config c;
    c.moduleId = id;
    c.table    = core::RegisterTable::HoldingRegister;
    c.address  = 500;
    c.values   = std::move(values);
    c.periodMs = periodMs;
    c.priority = sched::Priority::Low;
    return c;
}

} // namespace

TEST_CASE("Heartbeat onTick before start returns Cancelled",
          "[heartbeat][lifecycle]") {
    test::MockTransport mock;
    module::Heartbeat hb(cfgFor(), mock);
    auto r = hb.onTick();
    REQUIRE(r.kind == sched::ResultKind::Cancelled);
    REQUIRE(mock.capturedWrites().isEmpty());
}

TEST_CASE("Heartbeat first onTick after start writes immediately",
          "[heartbeat][initial]") {
    test::MockTransport mock;
    module::Heartbeat hb(cfgFor("hb.first", 1000), mock);
    hb.start();
    auto r = hb.onTick();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 1);
    REQUIRE(writes.first().startAddress == 500);
    REQUIRE(writes.first().values == QList<quint16>{0xA5A5});
}

TEST_CASE("Heartbeat respects periodMs between subsequent writes",
          "[heartbeat][period]") {
    test::MockTransport mock;
    module::Heartbeat hb(cfgFor("hb", 20), mock);
    hb.start();

    auto r1 = hb.onTick();
    REQUIRE(r1.kind == sched::ResultKind::Ok);
    REQUIRE(mock.readCount() == 0);
    REQUIRE(mock.capturedWrites().size() == 1);

    // Immediate retry — period not yet elapsed.
    auto r2 = hb.onTick();
    REQUIRE(r2.kind == sched::ResultKind::Ok);
    REQUIRE(mock.capturedWrites().size() == 1);   // no new write

    std::this_thread::sleep_for(25ms);
    auto r3 = hb.onTick();
    REQUIRE(r3.kind == sched::ResultKind::Ok);
    REQUIRE(mock.capturedWrites().size() == 2);
}

TEST_CASE("Heartbeat.driveTick writes via the async path and honours periodMs",
          "[heartbeat][async]") {
    test::MockTransport mock;
    module::Heartbeat hb(cfgFor("hb.async", 1000), mock);
    hb.start();

    hb.driveTick();
    REQUIRE(mock.capturedWrites().size() == 1);
    REQUIRE(mock.capturedWrites().first().values == QList<quint16>{0xA5A5});

    hb.driveTick();   // period not elapsed → no new write
    REQUIRE(mock.capturedWrites().size() == 1);
}

TEST_CASE("Heartbeat.driveTick coalesces while a write is in flight",
          "[heartbeat][async][coalesce]") {
    test::MockTransport mock;
    mock.setDeferAsync(true);
    module::Heartbeat hb(cfgFor("hb.coalesce", 0), mock);   // period 0 → always due
    hb.start();

    hb.driveTick();                       // starts write, deferred (in flight)
    REQUIRE(mock.capturedWrites().size() == 1);
    hb.driveTick();                       // in flight → skipped
    REQUIRE(mock.capturedWrites().size() == 1);

    REQUIRE(mock.completeNextWrite());    // finish it
    hb.driveTick();                       // free → a new write
    REQUIRE(mock.capturedWrites().size() == 2);
    REQUIRE(mock.completeNextWrite());
}

TEST_CASE("Heartbeat surfaces write failures from the transport",
          "[heartbeat][error]") {
    test::MockTransport mock;
    module::Heartbeat hb(cfgFor("hb"), mock);
    hb.start();

    transport::WriteResult fail; fail.ok = false; fail.errorMessage = "nope";
    mock.enqueueWriteResult(fail);

    auto r = hb.onTick();
    REQUIRE(r.kind == sched::ResultKind::Error);
    REQUIRE(r.errorMessage == "nope");
}
