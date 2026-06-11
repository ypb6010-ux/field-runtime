// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/sched/CreditScheduler.h"
#include "core/sched/PriorityScheduler.h"
#include "core/sched/RequestScheduler.h"

using namespace core::sched;
using namespace std::chrono_literals;

namespace {

RequestTag tagOf(QString id, Priority p = Priority::Normal,
                  bool interruptable = false) {
    RequestTag t;
    t.moduleId      = std::move(id);
    t.priority      = p;
    t.interruptable = interruptable;
    return t;
}

} // namespace

TEST_CASE("CreditScheduler allows up to maxInflight concurrent work units",
          "[sched][credit]") {
    SchedulerConfig cfg;
    cfg.maxInflight   = 4;
    cfg.maxQueueDepth = 64;
    CreditScheduler s(cfg);

    std::atomic<int> running{0};
    std::atomic<int> peakRunning{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            s.submit(tagOf(QStringLiteral("m%1").arg(i)), [&] {
                int cur = running.fetch_add(1) + 1;
                int prev = peakRunning.load();
                while (cur > prev
                    && !peakRunning.compare_exchange_weak(prev, cur)) {}
                std::this_thread::sleep_for(40ms);
                running.fetch_sub(1);
            });
        });
    }
    for (auto& t : threads) t.join();

    REQUIRE(peakRunning.load() > 1);
    REQUIRE(peakRunning.load() <= 4);
    REQUIRE(s.stats().totalCompleted == 8);
    REQUIRE(s.stats().inflight == 0);
}

TEST_CASE("PriorityScheduler preempts lower-priority interruptable peers",
          "[sched][priority][preempt]") {
    SchedulerConfig cfg;
    cfg.maxInflight        = 1;
    cfg.maxQueueDepth      = 64;
    PriorityScheduler s(cfg);

    std::atomic<bool>     blocker{true};
    std::atomic<int>      lowsAttempted{0};
    std::atomic<int>      lowsCancelled{0};
    std::atomic<int>      highRan{0};

    // Hog the single inflight slot from a separate thread so the rest
    // queue up behind it.
    std::thread hog([&] {
        s.submit(tagOf("hog", Priority::Normal), [&] {
            while (blocker.load()) std::this_thread::sleep_for(5ms);
        });
    });

    // Wait until the hog is actually running before queueing low entries.
    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline && s.stats().inflight < 1) {
        std::this_thread::sleep_for(5ms);
    }

    std::vector<std::thread> lows;
    for (int i = 0; i < 4; ++i) {
        lows.emplace_back([&, i] {
            ++lowsAttempted;
            auto r = s.submit(tagOf(QStringLiteral("low%1").arg(i),
                                     Priority::Low, /*interruptable=*/true),
                              [] { std::this_thread::sleep_for(50ms); });
            if (r.kind == ResultKind::Cancelled) ++lowsCancelled;
        });
    }

    // Allow the lows to queue.
    std::this_thread::sleep_for(60ms);

    // Land a high-priority request. preemptLowerInterruptables triggers,
    // cancelling all queued Low/interruptable entries.
    std::thread high([&] {
        auto r = s.submit(tagOf("emergency", Priority::Critical),
                          [&] { ++highRan; });
        REQUIRE(r.kind == ResultKind::Ok);
    });

    // Let the high request observe the queue, then release the hog so
    // the scheduler can drain everything.
    std::this_thread::sleep_for(40ms);
    blocker.store(false);

    hog.join();
    high.join();
    for (auto& t : lows) t.join();

    REQUIRE(lowsAttempted.load() == 4);
    REQUIRE(lowsCancelled.load() >= 1);
    REQUIRE(highRan.load() == 1);
}

TEST_CASE("PriorityScheduler starvation guard promotes long-waiting lanes",
          "[sched][priority][starvation]") {
    SchedulerConfig cfg;
    cfg.maxInflight        = 1;
    cfg.maxQueueDepth      = 256;
    cfg.starvationGuardMs  = 80;
    cfg.fifoWithinLane     = true;
    PriorityScheduler s(cfg);

    std::atomic<bool>          lowRan{false};
    std::atomic<bool>          stopHighs{false};

    std::thread lowThread([&] {
        auto r = s.submit(tagOf("low", Priority::Low), [&] { lowRan.store(true); });
        REQUIRE(r.kind == ResultKind::Ok);
    });

    // Brief pause to let the low request hit the queue first.
    std::this_thread::sleep_for(10ms);

    // Stream high-priority work that would normally starve the low lane.
    std::vector<std::thread> highs;
    for (int i = 0; i < 12; ++i) {
        highs.emplace_back([&] {
            while (!stopHighs.load()) {
                s.submit(tagOf("high", Priority::High),
                         [] { std::this_thread::sleep_for(15ms); });
                std::this_thread::sleep_for(2ms);
            }
        });
    }

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !lowRan.load()) {
        std::this_thread::sleep_for(10ms);
    }
    stopHighs.store(true);

    for (auto& t : highs) t.join();
    lowThread.join();

    REQUIRE(lowRan.load());
}

TEST_CASE("SerialScheduler stays at maxInflight=1 even when cfg says otherwise",
          "[sched][serial][factory]") {
    SchedulerConfig cfg;
    cfg.kind          = SchedulerKind::Serial;
    cfg.maxInflight   = 8;    // factory must clamp this to 1
    cfg.maxQueueDepth = 32;
    auto s = makeScheduler(cfg);

    std::atomic<int> peakRunning{0};
    std::atomic<int> running{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&] {
            s->submit(tagOf("m"), [&] {
                int cur = running.fetch_add(1) + 1;
                int prev = peakRunning.load();
                while (cur > prev
                    && !peakRunning.compare_exchange_weak(prev, cur)) {}
                std::this_thread::sleep_for(20ms);
                running.fetch_sub(1);
            });
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(peakRunning.load() == 1);
}

TEST_CASE("SchedulerStats reports p50/p99 from the latency window",
          "[sched][stats]") {
    SchedulerConfig cfg;
    cfg.maxInflight = 1;
    CreditScheduler s(cfg);

    // Submit fast work units; p50 ≈ small.
    for (int i = 0; i < 30; ++i) {
        s.submit(tagOf("m"), [] { std::this_thread::sleep_for(5ms); });
    }
    // Then a handful of slow units (>3 so the floor-indexed p99 of ~33
    // samples falls onto a slow one).
    for (int i = 0; i < 4; ++i) {
        s.submit(tagOf("m"), [] { std::this_thread::sleep_for(50ms); });
    }

    auto st = s.stats();
    REQUIRE(st.totalCompleted == 34);
    REQUIRE(st.p99LatencyMs >= 40);
    REQUIRE(st.p50LatencyMs <= st.p99LatencyMs);
}
