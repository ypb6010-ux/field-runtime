// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "core/module/SinkWindow.h"
#include "core/sched/SchedulerTypes.h"
#include "mocks/MockTransport.h"

using namespace core;
using namespace std::chrono_literals;

namespace {

module::SinkWindow::Config baseCfg(QString const& id   = "win",
                                   int            start = 0,
                                   int            size  = 4) {
    module::SinkWindow::Config c;
    c.moduleId      = id;
    c.table         = core::RegisterTable::HoldingRegister;
    c.startAddress  = start;
    c.size          = size;
    c.priority      = sched::Priority::High;
    c.debounceMs    = 10;
    c.keepAlivePeriodMs = 0;
    return c;
}

} // namespace

TEST_CASE("SinkWindow initial state matches configured initial values",
          "[sink][init]") {
    test::MockTransport mock;
    auto cfg = baseCfg();
    cfg.initial = {0x10, 0x20, 0x30, 0x40};

    module::SinkWindow w(cfg, mock);
    REQUIRE(w.size() == 4);
    REQUIRE(w.startAddress() == 0);
    REQUIRE(w.snapshot() == core::RegisterWords{0x10, 0x20, 0x30, 0x40});
    REQUIRE_FALSE(w.dirty());
}

TEST_CASE("stageRegister updates only when value differs", "[sink][stage]") {
    test::MockTransport mock;
    module::SinkWindow w(baseCfg(), mock);

    REQUIRE(w.stageRegister(0, 0x42));
    REQUIRE(w.dirty());
    REQUIRE(w.snapshot().at(0) == 0x42);

    REQUIRE_FALSE(w.stageRegister(0, 0x42));   // identical value → no-op
    REQUIRE(w.snapshot().at(0) == 0x42);
}

TEST_CASE("stageRegister honours masks for bit writes", "[sink][mask]") {
    test::MockTransport mock;
    module::SinkWindow w(baseCfg(), mock);

    REQUIRE(w.stageRegister(1, 0xFFFF, /*mask*/0x00FF));   // low byte only
    REQUIRE(w.snapshot().at(1) == 0x00FF);

    REQUIRE(w.stageRegister(1, 0xAA00, /*mask*/0xFF00));   // high byte
    REQUIRE(w.snapshot().at(1) == 0xAAFF);

    // Clear bit 3 first (it's currently set as part of 0xAAFF) so we can
    // verify the bit-mask path actually flips it back on.
    REQUIRE(w.stageRegister(1, 0x0000, /*mask*/0x0008));
    REQUIRE(w.snapshot().at(1) == quint16(0xAAFF & ~0x0008));

    REQUIRE(w.stageRegister(1, 0xFFFF, /*mask*/0x0008));
    REQUIRE(w.snapshot().at(1) == quint16(0xAAFF));
}

TEST_CASE("stageRegister outside range is silently ignored", "[sink][safety]") {
    test::MockTransport mock;
    module::SinkWindow w(baseCfg("win", 100, 4), mock);

    REQUIRE_FALSE(w.stageRegister(50,  1));      // before range
    REQUIRE_FALSE(w.stageRegister(104, 1));      // past end
    REQUIRE_FALSE(w.dirty());
}

TEST_CASE("onTick before start returns Cancelled and writes nothing",
          "[sink][lifecycle]") {
    test::MockTransport mock;
    module::SinkWindow w(baseCfg(), mock);
    REQUIRE(w.stageRegister(0, 1));
    auto r = w.onTick();
    REQUIRE(r.kind == sched::ResultKind::Cancelled);
    REQUIRE(mock.capturedWrites().isEmpty());
}

TEST_CASE("onTick coalesces multiple stages into one write after debounce",
          "[sink][debounce][coalesce]") {
    test::MockTransport mock;
    auto cfg = baseCfg();
    cfg.debounceMs = 15;
    module::SinkWindow w(cfg, mock);
    w.start();

    w.stageRegister(0, 0xAAAA);
    w.stageRegister(1, 0xBBBB);
    w.stageRegister(2, 0xCCCC);

    // Pre-debounce: no flush
    auto r1 = w.onTick();
    REQUIRE(r1.kind == sched::ResultKind::Ok);
    REQUIRE(mock.capturedWrites().isEmpty());

    std::this_thread::sleep_for(20ms);

    auto r2 = w.onTick();
    REQUIRE(r2.kind == sched::ResultKind::Ok);
    auto const writes = mock.capturedWrites();   // hold by value — `first()`
                                                  // on a temporary leaves a
                                                  // dangling reference.
    REQUIRE(writes.size() == 1);
    REQUIRE(writes.first().startAddress == 0);
    REQUIRE(writes.first().values == core::RegisterWords{0xAAAA, 0xBBBB, 0xCCCC, 0});
    REQUIRE_FALSE(w.dirty());
}

TEST_CASE("onTick with no changes is a no-op", "[sink][idle]") {
    test::MockTransport mock;
    module::SinkWindow w(baseCfg(), mock);
    w.start();
    auto r = w.onTick();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    REQUIRE(mock.capturedWrites().isEmpty());
}

TEST_CASE("keepAlivePeriod fires periodic writes even without changes",
          "[sink][keepalive]") {
    test::MockTransport mock;
    auto cfg = baseCfg();
    cfg.keepAlivePeriodMs = 20;
    cfg.initial = {0xDEAD, 0xBEEF, 0, 0};
    module::SinkWindow w(cfg, mock);
    w.start();

    auto r1 = w.onTick();              // just after start: no flush
    REQUIRE(mock.capturedWrites().isEmpty());

    std::this_thread::sleep_for(25ms);

    auto r2 = w.onTick();              // keepAlive expired → flush
    REQUIRE(r2.kind == sched::ResultKind::Ok);
    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 1);
    REQUIRE(writes.first().values == core::RegisterWords{0xDEAD, 0xBEEF, 0, 0});
}

TEST_CASE("forceFlush bypasses debounce and keepalive timing",
          "[sink][force]") {
    test::MockTransport mock;
    auto cfg = baseCfg();
    cfg.debounceMs = 10'000;            // effectively never via debounce
    cfg.keepAlivePeriodMs = 0;
    module::SinkWindow w(cfg, mock);
    w.start();

    w.stageRegister(0, 0x99);
    w.forceFlush();

    auto r = w.onTick();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 1);
    REQUIRE(writes.first().values.at(0) == 0x99);
    REQUIRE_FALSE(w.dirty());
}

TEST_CASE("Write failure preserves dirty state for next-tick retry",
          "[sink][retry]") {
    test::MockTransport mock;
    auto cfg = baseCfg();
    cfg.debounceMs = 5;
    module::SinkWindow w(cfg, mock);
    w.start();

    w.stageRegister(0, 0x55);
    std::this_thread::sleep_for(10ms);

    transport::WriteResult fail; fail.ok = false; fail.errorMessage = "boom";
    mock.enqueueWriteResult(fail);

    auto r1 = w.onTick();
    REQUIRE(r1.kind == sched::ResultKind::Error);
    REQUIRE(r1.errorMessage.contains("boom"));
    REQUIRE(w.dirty());                 // dirty preserved for retry

    // Now allow success on the next attempt.
    auto r2 = w.onTick();
    REQUIRE(r2.kind == sched::ResultKind::Ok);
    REQUIRE_FALSE(w.dirty());
    REQUIRE(mock.capturedWrites().size() == 2);
}

TEST_CASE("SinkWindow.driveTick flushes via the async path", "[sink][async]") {
    test::MockTransport mock;
    auto cfg = baseCfg("win.async");
    cfg.debounceMs = 10'000;            // only forceFlush will trigger
    module::SinkWindow w(cfg, mock);
    w.start();

    w.stageRegister(0, 0x1234);
    w.forceFlush();
    w.driveTick();                     // sync mock async → flush completes inline

    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 1);
    REQUIRE(writes.first().values.at(0) == 0x1234);
    REQUIRE_FALSE(w.dirty());           // cleared on a successful flush
}

TEST_CASE("SinkWindow.driveTick coalesces and does not lose a mid-flight stage",
          "[sink][async][coalesce]") {
    test::MockTransport mock;
    mock.setDeferAsync(true);
    auto cfg = baseCfg("win.coalesce");
    cfg.debounceMs = 0;                     // dirty flushes on the next tick
    module::SinkWindow w(cfg, mock);
    w.start();

    w.stageRegister(0, 0x11);
    w.driveTick();                          // flush 0x11, deferred (in flight)
    REQUIRE(mock.capturedWrites().size() == 1);
    REQUIRE(mock.capturedWrites().first().values.at(0) == 0x11);

    w.stageRegister(0, 0x22);               // new stage WHILE the write is in flight
    w.driveTick();                          // in flight → coalesced (skipped)
    REQUIRE(mock.capturedWrites().size() == 1);

    REQUIRE(mock.completeNextWrite());      // first flush done; 0x22 must NOT be lost
    w.driveTick();                          // → flushes 0x22
    REQUIRE(mock.capturedWrites().size() == 2);
    REQUIRE(mock.capturedWrites().at(1).values.at(0) == 0x22);

    REQUIRE(mock.completeNextWrite());
    REQUIRE_FALSE(w.dirty());               // now everything is flushed
}

TEST_CASE("SinkWindow uses moduleId and priority on the scheduler tag",
          "[sink][sched]") {
    test::MockTransport mock;
    auto cfg = baseCfg("win.PLC1.control");
    module::SinkWindow w(cfg, mock);
    w.start();
    w.stageRegister(0, 1);
    std::this_thread::sleep_for(15ms);
    auto r = w.onTick();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    // Tag plumbing is verified indirectly through SerialSchedulerTests; here
    // we just confirm the module exposes the right metadata.
    REQUIRE(w.id() == "win.PLC1.control");
    REQUIRE(w.priority() == sched::Priority::High);
}
