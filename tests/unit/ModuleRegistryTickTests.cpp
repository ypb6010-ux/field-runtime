// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>

#include "core/module/FunctionalModule.h"
#include "core/module/ModuleRegistry.h"

using namespace core::module;
using namespace std::chrono_literals;

namespace {

// Bare module that counts ticks. Used to verify the timer wiring without
// dragging a real transport into the test.
class CountingModule : public FunctionalModule {
public:
    CountingModule(std::string id, int periodMs) : m_periodMs(periodMs) {
        m_id          = std::move(id);
        m_transportId = "mock";
    }
    int  tickPeriodMs() const override { return m_periodMs; }
    void driveTick()         override { ticks.fetch_add(1, std::memory_order_acq_rel); }
    void start()  override { started = true; }
    void stop()   override { started = false; }
    void pause()  override {}
    void resume() override {}
    std::atomic<int> ticks{0};
    bool             started = false;
private:
    int m_periodMs;
};

} // namespace

TEST_CASE("ModuleRegistry::startAll arms a timer per module and ticks fire",
          "[module-registry][autotick]") {
    ModuleRegistry reg;
    auto* m = new CountingModule("counter", 20);
    reg.registerModule(std::unique_ptr<FunctionalModule>(m));

    reg.startAll();
    REQUIRE(m->started);

    // Spin a local event loop briefly so the timer fires.
    auto* app = QCoreApplication::instance();
    REQUIRE(app != nullptr);
    QElapsedTimer t; t.start();
    while (t.elapsed() < 200 && m->ticks.load() < 3) {
        app->processEvents(QEventLoop::AllEvents, 10);
    }
    REQUIRE(m->ticks.load() >= 2);

    reg.stopAll();
    int const after = m->ticks.load();
    QElapsedTimer t2; t2.start();
    while (t2.elapsed() < 80) app->processEvents(QEventLoop::AllEvents, 10);
    // No further ticks after stop.
    REQUIRE(m->ticks.load() == after);
}

TEST_CASE("ModuleRegistry::setAutoTickEnabled(false) suppresses the timer",
          "[module-registry][autotick]") {
    ModuleRegistry reg;
    auto* m = new CountingModule("counter.off", 20);
    reg.registerModule(std::unique_ptr<FunctionalModule>(m));
    reg.setAutoTickEnabled(false);

    reg.startAll();
    REQUIRE(m->started);

    auto* app = QCoreApplication::instance();
    QElapsedTimer t; t.start();
    while (t.elapsed() < 150) app->processEvents(QEventLoop::AllEvents, 10);
    REQUIRE(m->ticks.load() == 0);

    reg.stopAll();
}

TEST_CASE("ModuleRegistry rejects registerModule once ticking is live",
          "[module-registry][lifecycle]") {
    ModuleRegistry reg;
    REQUIRE(reg.registerModule(
        std::make_unique<CountingModule>("a", 20)));

    reg.startAll();
    // While started, a tick timer holds a raw module pointer — registering
    // (and thus possibly replacing/destroying a module) must be refused.
    REQUIRE_FALSE(reg.registerModule(
        std::make_unique<CountingModule>("b", 20)));
    REQUIRE(reg.find("b") == nullptr);

    reg.stopAll();
    // After stop, registration is allowed again.
    REQUIRE(reg.registerModule(
        std::make_unique<CountingModule>("c", 20)));
    REQUIRE(reg.find("c") != nullptr);
}

TEST_CASE("ModuleRegistry skips modules whose tickPeriodMs is 0",
          "[module-registry][autotick]") {
    ModuleRegistry reg;
    auto* a = new CountingModule("p.zero", 0);
    auto* b = new CountingModule("p.live", 20);
    reg.registerModule(std::unique_ptr<FunctionalModule>(a));
    reg.registerModule(std::unique_ptr<FunctionalModule>(b));

    reg.startAll();
    auto* app = QCoreApplication::instance();
    QElapsedTimer t; t.start();
    while (t.elapsed() < 200 && b->ticks.load() < 3) {
        app->processEvents(QEventLoop::AllEvents, 10);
    }
    REQUIRE(a->ticks.load() == 0);
    REQUIRE(b->ticks.load() >= 2);
    reg.stopAll();
}
