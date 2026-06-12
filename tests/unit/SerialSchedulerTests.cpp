// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/sched/SerialScheduler.h"

using namespace core::sched;
using namespace std::chrono_literals;

namespace {

RequestTag tagFor(std::string moduleId,
                  Priority    priority = Priority::Normal) {
    RequestTag t;
    t.moduleId = std::move(moduleId);
    t.priority = priority;
    return t;
}

// Wait for a predicate to become true or fail with a context-bearing message.
template <class Pred>
void waitFor(Pred&& p, std::chrono::milliseconds budget = 2s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (!p()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            FAIL("Timed out waiting for predicate");
        }
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace

TEST_CASE("SerialScheduler runs idle work synchronously", "[sched][serial]") {
    SerialScheduler s(SchedulerConfig{});
    bool ran = false;
    auto r = s.submit(tagFor("a"), [&] { ran = true; });
    REQUIRE(r.kind == ResultKind::Ok);
    REQUIRE(ran);
    REQUIRE(s.stats().totalSubmitted == 1);
    REQUIRE(s.stats().totalCompleted == 1);
}

TEST_CASE("SerialScheduler never has more than one inflight at once",
          "[sched][serial][concurrent]") {
    SerialScheduler s(SchedulerConfig{});

    std::atomic<int> active{0};
    std::atomic<int> peakActive{0};
    constexpr int    kThreads = 8;
    constexpr int    kPerT    = 10;

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            for (int j = 0; j < kPerT; ++j) {
                s.submit(tagFor(std::to_string(i)), [&] {
                    int cur = active.fetch_add(1) + 1;
                    int prev = peakActive.load();
                    while (cur > prev
                        && !peakActive.compare_exchange_weak(prev, cur)) {}
                    std::this_thread::sleep_for(1ms);
                    active.fetch_sub(1);
                });
            }
        });
    }
    for (auto& t : ts) t.join();

    REQUIRE(peakActive.load() == 1);
    REQUIRE(s.stats().totalCompleted == std::uint64_t(kThreads * kPerT));
}

TEST_CASE("SerialScheduler dispatches Critical priority ahead of Normal",
          "[sched][serial][priority]") {
    SchedulerConfig cfg;
    cfg.fifoWithinLane = true;
    SerialScheduler s(cfg);

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    firstStarted = false;
    bool                    releaseFirst = false;
    std::vector<std::string> order;

    std::thread t1([&] {
        s.submit(tagFor("first", Priority::Normal), [&] {
            { std::lock_guard lk(mtx); firstStarted = true; }
            cv.notify_all();
            std::unique_lock lk(mtx);
            cv.wait(lk, [&] { return releaseFirst; });
            order.push_back("first");
        });
    });
    // Wait for the in-flight slot to be taken.
    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return firstStarted; });
    }

    std::thread t2([&] {
        s.submit(tagFor("normal", Priority::Normal), [&] {
            std::lock_guard lk(mtx); order.push_back("normal");
        });
    });
    std::thread t3([&] {
        s.submit(tagFor("crit", Priority::Critical), [&] {
            std::lock_guard lk(mtx); order.push_back("critical");
        });
    });

    // Both must reach the queue before we release the first task.
    waitFor([&] { return s.stats().queueDepth >= 2; });

    { std::lock_guard lk(mtx); releaseFirst = true; }
    cv.notify_all();
    t1.join();
    t2.join();
    t3.join();

    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == "first");
    REQUIRE(order[1] == "critical");
    REQUIRE(order[2] == "normal");
}

TEST_CASE("SerialScheduler round-robins within a lane when configured",
          "[sched][serial][round-robin]") {
    SchedulerConfig cfg;
    cfg.fifoWithinLane = false;            // enable round-robin
    SerialScheduler s(cfg);

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    firstStarted = false;
    bool                    releaseFirst = false;
    std::vector<std::string> order;

    std::thread t1([&] {
        s.submit(tagFor("seed"), [&] {
            { std::lock_guard lk(mtx); firstStarted = true; }
            cv.notify_all();
            std::unique_lock lk(mtx);
            cv.wait(lk, [&] { return releaseFirst; });
        });
    });
    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return firstStarted; });
    }

    // Queue: A, A, B, B  (all Normal priority).
    // Round-robin should dispatch them as A, B, A, B.
    std::thread tA1([&] {
        s.submit(tagFor("A"), [&] {
            std::lock_guard lk(mtx); order.push_back("A");
        });
    });
    std::thread tA2([&] {
        s.submit(tagFor("A"), [&] {
            std::lock_guard lk(mtx); order.push_back("A");
        });
    });
    std::thread tB1([&] {
        s.submit(tagFor("B"), [&] {
            std::lock_guard lk(mtx); order.push_back("B");
        });
    });
    std::thread tB2([&] {
        s.submit(tagFor("B"), [&] {
            std::lock_guard lk(mtx); order.push_back("B");
        });
    });
    waitFor([&] { return s.stats().queueDepth >= 4; });

    // Release seed; rest dispatch.
    { std::lock_guard lk(mtx); releaseFirst = true; }
    cv.notify_all();

    t1.join(); tA1.join(); tA2.join(); tB1.join(); tB2.join();

    REQUIRE(order == std::vector<std::string>{"A", "B", "A", "B"});
}

TEST_CASE("SerialScheduler enforces inter-request gap between submissions",
          "[sched][serial][gap]") {
    SchedulerConfig cfg;
    cfg.interRequestGapMs = 30;
    SerialScheduler s(cfg);

    auto t0 = std::chrono::steady_clock::now();
    s.submit(tagFor("a"), [] {});
    auto t1 = std::chrono::steady_clock::now();
    s.submit(tagFor("a"), [] {});
    auto t2 = std::chrono::steady_clock::now();

    auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
    // Allow a small scheduler-overhead slack but the gap floor must hold.
    REQUIRE(gap.count() >= 25);
    REQUIRE(t1 > t0);          // first submit not affected by gap
}

TEST_CASE("SerialScheduler.cancelModule unblocks pending entries",
          "[sched][serial][cancel]") {
    SerialScheduler s(SchedulerConfig{});

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    seedStarted = false;
    bool                    release     = false;

    std::thread t1([&] {
        s.submit(tagFor("seed"), [&] {
            { std::lock_guard lk(mtx); seedStarted = true; }
            cv.notify_all();
            std::unique_lock lk(mtx);
            cv.wait(lk, [&] { return release; });
        });
    });
    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return seedStarted; });
    }

    SubmitResult pendingResult;
    bool         pendingDone = false;
    std::thread t2([&] {
        pendingResult = s.submit(tagFor("doomed"), [] { FAIL("must not run"); });
        std::lock_guard lk(mtx);
        pendingDone = true;
        cv.notify_all();
    });
    waitFor([&] { return s.stats().queueDepth == 1; });

    int cancelled = s.cancelModule("doomed");
    REQUIRE(cancelled == 1);

    // The pending submit() should wake and return Cancelled.
    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return pendingDone; });
    }
    REQUIRE(pendingResult.kind == ResultKind::Cancelled);

    { std::lock_guard lk(mtx); release = true; }
    cv.notify_all();
    t1.join(); t2.join();

    REQUIRE(s.stats().totalCancelled == 1);
}

TEST_CASE("SerialScheduler.cancelModule returns 0 when nothing matches",
          "[sched][serial][cancel]") {
    SerialScheduler s(SchedulerConfig{});
    REQUIRE(s.cancelModule("nothing") == 0);
}

TEST_CASE("SerialScheduler refuses submissions once queue is full",
          "[sched][serial][backpressure]") {
    SchedulerConfig cfg;
    cfg.maxQueueDepth = 2;
    SerialScheduler s(cfg);

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    started = false;
    bool                    release = false;

    std::thread t1([&] {
        s.submit(tagFor("seed"), [&] {
            { std::lock_guard lk(mtx); started = true; }
            cv.notify_all();
            std::unique_lock lk(mtx);
            cv.wait(lk, [&] { return release; });
        });
    });
    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return started; });
    }
    // Slot is held. Queue depth limit is 2 (counts inflight). Next submit fills
    // the queue; the one after that should be rejected.
    std::thread t2([&] { s.submit(tagFor("a"), [] {}); });
    waitFor([&] { return s.stats().queueDepth >= 1; });

    auto r = s.submit(tagFor("b"), [] {});
    REQUIRE(r.kind == ResultKind::Error);
    REQUIRE(r.errorMessage.find("queue full") != std::string::npos);

    { std::lock_guard lk(mtx); release = true; }
    cv.notify_all();
    t1.join(); t2.join();
}

TEST_CASE("SerialScheduler circuit breaker opens after consecutive failures",
          "[sched][serial][circuit]") {
    SchedulerConfig cfg;
    cfg.circuitBreakerThreshold = 3;
    cfg.circuitBreakerOpenMs    = 50;
    SerialScheduler s(cfg);

    for (int i = 0; i < 3; ++i) {
        auto r = s.submit(tagFor("a"), [] {
            throw std::runtime_error("boom");
        });
        REQUIRE(r.kind == ResultKind::Error);
    }
    REQUIRE(s.stats().circuitState == CircuitState::Open);
    REQUIRE(s.stats().totalFailed  == 3);

    auto r = s.submit(tagFor("a"), [] { FAIL("must not run"); });
    REQUIRE(r.kind == ResultKind::CircuitOpen);

    // After openMs elapses the breaker turns half-open and admits a probe.
    std::this_thread::sleep_for(70ms);
    bool ranProbe = false;
    auto probe = s.submit(tagFor("a"), [&] { ranProbe = true; });
    REQUIRE(probe.kind == ResultKind::Ok);
    REQUIRE(ranProbe);
    REQUIRE(s.stats().circuitState == CircuitState::Closed);
}

TEST_CASE("SerialScheduler stats reflect lane depth and totals",
          "[sched][serial][stats]") {
    SerialScheduler s(SchedulerConfig{});

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    started = false;
    bool                    release = false;

    std::thread t1([&] {
        s.submit(tagFor("seed"), [&] {
            { std::lock_guard lk(mtx); started = true; }
            cv.notify_all();
            std::unique_lock lk(mtx);
            cv.wait(lk, [&] { return release; });
        });
    });
    { std::unique_lock lk(mtx); cv.wait(lk, [&] { return started; }); }

    std::thread t2([&] { s.submit(tagFor("low", Priority::Low),    [] {}); });
    std::thread t3([&] { s.submit(tagFor("crit", Priority::Critical), [] {}); });
    waitFor([&] { return s.stats().queueDepth >= 2; });

    auto snap = s.stats();
    REQUIRE(snap.inflight == 1);
    REQUIRE(snap.queueDepth == 2);
    REQUIRE(snap.laneQueueDepth[static_cast<int>(Priority::Low)]      == 1);
    REQUIRE(snap.laneQueueDepth[static_cast<int>(Priority::Critical)] == 1);

    { std::lock_guard lk(mtx); release = true; }
    cv.notify_all();
    t1.join(); t2.join(); t3.join();

    auto final = s.stats();
    REQUIRE(final.queueDepth     == 0);
    REQUIRE(final.totalSubmitted == 3);
    REQUIRE(final.totalCompleted == 3);
}
