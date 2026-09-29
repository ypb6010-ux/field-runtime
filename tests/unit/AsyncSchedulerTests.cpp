// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/sched/RequestScheduler.h"
#include "core/sched/SchedulerTypes.h"
#include "core/sched/SerialScheduler.h"

using namespace core::sched;
using namespace std::chrono_literals;

namespace {

// A manual async op: records the order it was started in and parks its
// completion callback so the test can complete it on demand.
struct Harness {
    std::unique_ptr<RequestScheduler> sched;
    std::vector<int>                  started;
    std::array<AsyncDone, 8>          done{};

    explicit Harness(SchedulerConfig cfg) : sched(makeScheduler(cfg)) {}

    SubmitResult submit(int id, Priority pri = Priority::Normal) {
        RequestTag tag;
        tag.moduleId = std::to_string(id);
        tag.priority = pri;
        return sched->submitAsync(tag, [this, id](AsyncDone d) {
            started.push_back(id);
            done[id] = std::move(d);
        });
    }
};

} // namespace

TEST_CASE("submitAsync runs work and completes via the done callback",
          "[sched][async]") {
    Harness h(SchedulerConfig{});   // Serial: maxInflight forced to 1

    REQUIRE(h.submit(0).kind == ResultKind::Ok);
    REQUIRE(h.started == std::vector<int>{0});
    REQUIRE(h.sched->stats().inflight == 1);

    h.done[0](true);
    auto st = h.sched->stats();
    REQUIRE(st.totalSubmitted == 1);
    REQUIRE(st.totalCompleted == 1);
    REQUIRE(st.inflight == 0);
}

TEST_CASE("submitAsync serialises with one in flight (maxInflight=1)",
          "[sched][async]") {
    Harness h(SchedulerConfig{});

    REQUIRE(h.submit(0).kind == ResultKind::Ok);   // starts now
    REQUIRE(h.submit(1).kind == ResultKind::Ok);   // queued, not started
    REQUIRE(h.started == std::vector<int>{0});
    REQUIRE(h.sched->stats().queueDepth == 1);

    h.done[0](true);                               // frees the slot → op1 runs
    REQUIRE(h.started == std::vector<int>{0, 1});
    REQUIRE(h.sched->stats().inflight == 1);

    h.done[1](true);
    REQUIRE(h.sched->stats().inflight == 0);
}

TEST_CASE("submitAsync serves a higher priority queued request first",
          "[sched][async]") {
    Harness h(SchedulerConfig{});

    h.submit(0, Priority::Normal);   // starts
    h.submit(1, Priority::Normal);   // queued
    h.submit(2, Priority::High);     // queued, should jump ahead of op1
    REQUIRE(h.started == std::vector<int>{0});

    h.done[0](true);
    REQUIRE(h.started == std::vector<int>{0, 2});   // High before Normal
    h.done[2](true);
    REQUIRE(h.started == std::vector<int>{0, 2, 1});
    h.done[1](true);
    REQUIRE(h.sched->stats().inflight == 0);
}

TEST_CASE("submitAsync opens the circuit after repeated failures and rejects",
          "[sched][async]") {
    SchedulerConfig cfg;
    cfg.circuitBreakerThreshold = 2;
    Harness h(cfg);

    h.submit(0);
    h.done[0](false);     // failure 1
    h.submit(1);
    h.done[1](false);     // failure 2 → circuit opens
    REQUIRE(h.sched->stats().circuitState == CircuitState::Open);

    auto rejected = h.submit(2);
    REQUIRE(rejected.kind == ResultKind::CircuitOpen);
    REQUIRE(h.started == std::vector<int>{0, 1});   // op2 never started
}

TEST_CASE("submitAsync defers the next request by inter_request_gap via delayFn",
          "[sched][async]") {
    SchedulerConfig cfg;
    cfg.interRequestGapMs = 40;
    Harness h(cfg);

    int                   scheduledMs = 0;
    std::function<void()> scheduledFn;
    h.sched->setDelayFn([&](int ms, std::function<void()> fn) {
        scheduledMs = ms;
        scheduledFn = std::move(fn);
    });

    h.submit(0);
    h.done[0](true);            // completes → starts the gap window
    h.submit(1);                // within the gap → must be deferred, not started
    REQUIRE(h.started == std::vector<int>{0});
    REQUIRE(scheduledMs > 0);
    REQUIRE(static_cast<bool>(scheduledFn));

    std::this_thread::sleep_for(55ms);   // let the gap elapse
    scheduledFn();                       // the deferred pump fires
    REQUIRE(h.started == std::vector<int>{0, 1});
    h.done[1](true);
}

TEST_CASE("a scheduler rejects mixing the sync and async submit paths",
          "[sched][async]") {
    RequestTag tag;
    tag.moduleId = "m";

    SECTION("sync first, then async is rejected") {
        auto s = makeScheduler(SchedulerConfig{});
        REQUIRE(s->submit(tag, [] {}).kind == ResultKind::Ok);
        REQUIRE(s->submitAsync(tag, [](AsyncDone d) { d(true); }).kind
                == ResultKind::Error);
    }
    SECTION("async first, then sync is rejected") {
        auto s = makeScheduler(SchedulerConfig{});
        REQUIRE(s->submitAsync(tag, [](AsyncDone d) { d(true); }).kind
                == ResultKind::Ok);
        REQUIRE(s->submit(tag, [] {}).kind == ResultKind::Error);
    }
}

TEST_CASE("submitAsync recovers the in-flight slot when the work throws",
          "[sched][async]") {
    Harness h(SchedulerConfig{});

    RequestTag tag;
    tag.moduleId = "thrower";
    auto r = h.sched->submitAsync(tag, [](AsyncDone) {
        throw std::runtime_error("boom");
    });
    REQUIRE(r.kind == ResultKind::Ok);   // accepted; the throw is handled internally
    auto st = h.sched->stats();
    REQUIRE(st.inflight == 0);            // slot was not leaked
    REQUIRE(st.totalFailed == 1);

    // A subsequent request still runs (the serial slot is free).
    REQUIRE(h.submit(1).kind == ResultKind::Ok);
    REQUIRE(h.started == std::vector<int>{1});
    h.done[1](true);
    REQUIRE(h.sched->stats().inflight == 0);
}

TEST_CASE("submitAsync ignores a duplicate completion", "[sched][async]") {
    Harness h(SchedulerConfig{});
    h.submit(0);
    h.done[0](true);
    h.done[0](true);   // double-fire — must be ignored
    auto st = h.sched->stats();
    REQUIRE(st.totalCompleted == 1);
    REQUIRE(st.inflight == 0);
}

TEST_CASE("stopAsync halts pumping of queued work (teardown safety)",
          "[sched][async]") {
    Harness h(SchedulerConfig{});
    h.submit(0);   // starts (in flight)
    h.submit(1);   // queued
    REQUIRE(h.started == std::vector<int>{0});

    static_cast<SerialScheduler*>(h.sched.get())->stopAsync();

    h.done[0](true);   // completes op0; the pump must NOT start op1
    REQUIRE(h.started == std::vector<int>{0});   // op1 never started after stop
    REQUIRE(h.sched->stats().inflight == 0);
    REQUIRE(h.sched->stats().queueDepth == 0);
    REQUIRE(h.sched->stats().totalCancelled == 1);

    RequestTag stoppedTag;
    stoppedTag.moduleId = "stopped";
    auto rejected = h.sched->submitAsync(
        stoppedTag, [](AsyncDone) { FAIL("must not run while stopped"); });
    REQUIRE(rejected.kind == ResultKind::Cancelled);

    static_cast<SerialScheduler*>(h.sched.get())->startAsync();
    REQUIRE(h.submit(2).kind == ResultKind::Ok);
    REQUIRE(h.started == std::vector<int>{0, 2});
    h.done[2](true);
}


TEST_CASE("Async replacement works at capacity and finishes outside the lock", "[sched][async][feedback]") {
    SchedulerConfig cfg;
    cfg.maxQueueDepth = 2;
    auto scheduler = makeScheduler(cfg);
    AsyncDone release;
    scheduler->submitAsync({}, [&](auto done) { release = std::move(done); });
    std::vector<ResultKind> results;
    RequestTag tag;
    tag.coalesceKey = "register:1:7";
    tag.onFinished = [&](auto kind) {
        scheduler->stats(); // callback must be able to re-enter scheduler
        results.push_back(kind);
    };
    int sent = 0;
    REQUIRE(scheduler->submitAsync(tag, [&](auto done) { sent = 1; done(true); }).kind == ResultKind::Ok);
    REQUIRE(scheduler->submitAsync(tag, [&](auto done) { sent = 2; done(true); }).kind == ResultKind::Ok);
    REQUIRE(results == std::vector<ResultKind>{ResultKind::Superseded});
    release(true);
    REQUIRE(sent == 2);
    REQUIRE(results == std::vector<ResultKind>{ResultKind::Superseded, ResultKind::Ok});
}

TEST_CASE("Async feedback covers preemption stop and throwing work exactly once", "[sched][async][feedback]") {
    auto scheduler = makeScheduler({});
    AsyncDone release;
    scheduler->submitAsync({}, [&](auto done) { release = std::move(done); });
    std::vector<ResultKind> results;
    RequestTag low;
    low.priority = Priority::Low;
    low.interruptable = true;
    low.onFinished = [&](auto r) { results.push_back(r); };
    scheduler->submitAsync(low, [](auto done) { done(true); });
    RequestTag high;
    high.priority = Priority::High;
    high.onFinished = low.onFinished;
    scheduler->submitAsync(high, [](auto done) { done(true); });
    REQUIRE(results == std::vector<ResultKind>{ResultKind::Cancelled});
    scheduler->stopAsync();
    REQUIRE(results == std::vector<ResultKind>{ResultKind::Cancelled, ResultKind::Cancelled});
    release(true);
    scheduler->startAsync();
    scheduler->submitAsync(high, [](auto) { throw std::runtime_error("write exception"); });
    REQUIRE(results.back() == ResultKind::Error);
    REQUIRE(results.size() == 3);
    scheduler->submitAsync(high, [](auto done) { done(true); done(false); });
    REQUIRE(results.back() == ResultKind::Ok);
    REQUIRE(results.size() == 4);
}
