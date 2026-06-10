// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/sched/SerialScheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace core::sched {

namespace {

enum class PendingState {
    Queued,
    Selected,
    Cancelled,
};

struct PendingEntry {
    RequestTag                              tag;
    PendingState                            state = PendingState::Queued;
    std::chrono::steady_clock::time_point   queuedAt;
    // Set for entries submitted via submitAsync(); empty for the sync path.
    AsyncWork                               asyncWork;
    std::chrono::steady_clock::time_point   startAt;   // when the work was invoked
    bool                                    completed = false;  // idempotent guard
};

constexpr int laneIndex(Priority p) noexcept { return static_cast<int>(p); }

// Bounded ring buffer for the most recent N completed latencies; p50/p99 are
// computed by a sort on demand. N is small (≤256), so sort cost is trivial
// and we avoid the t-digest / reservoir machinery for now.
class LatencyWindow {
public:
    explicit LatencyWindow(int capacity) : m_cap(capacity > 0 ? capacity : 1) {}
    void push(int latencyMs) {
        if (int(m_samples.size()) >= m_cap) m_samples.pop_front();
        m_samples.push_back(latencyMs);
    }
    int percentile(double q) const {
        if (m_samples.empty()) return 0;
        std::vector<int> tmp(m_samples.begin(), m_samples.end());
        std::sort(tmp.begin(), tmp.end());
        size_t idx = size_t(q * (tmp.size() - 1));
        if (idx >= tmp.size()) idx = tmp.size() - 1;
        return tmp[idx];
    }
private:
    int                m_cap;
    std::deque<int>    m_samples;
};

} // namespace

class SerialScheduler::Impl {
public:
    explicit Impl(SchedulerConfig c)
        : cfg(std::move(c))
        , latencyWindow(256) {
        if (cfg.maxInflight < 1) cfg.maxInflight = 1;
    }

    SchedulerConfig                                                   cfg;
    mutable std::mutex                                                mtx;
    std::condition_variable                                           cv;
    std::array<std::deque<std::shared_ptr<PendingEntry>>, kPriorityCount> lanes;
    std::array<QString, kPriorityCount>                               lastServedModule;
    int                                                               inflightCount = 0;
    std::chrono::steady_clock::time_point                             lastCompleteAt;
    LatencyWindow                                                     latencyWindow;

    quint64 totalSubmitted = 0;
    quint64 totalCompleted = 0;
    quint64 totalFailed    = 0;
    quint64 totalCancelled = 0;
    quint64 totalTimedOut  = 0;
    int     lastLatencyMs  = 0;

    CircuitState                          circuitState = CircuitState::Closed;
    int                                   errorStreak  = 0;
    std::chrono::steady_clock::time_point circuitOpenedAt;

    // Async (event-driven) path state.
    DelayFn  delayFn;                  // deferred-pump hook (for inter_request_gap)
    bool     delayScheduled = false;   // a gap timer is pending
    bool     pumping        = false;   // a pump is running (re-entrancy trampoline)
    bool     repump         = false;   // a pump was requested while one was running
    bool     stopped        = false;   // async path halted (teardown) — no new work

    // A scheduler instance is used in ONE mode: either the blocking submit()
    // path or the event-driven submitAsync() path — never both. Mixing them on
    // one instance would let one pump select the other's entries. The first
    // submit fixes the mode; a cross-mode call is rejected.
    enum class Mode { Unset, Sync, Async };
    Mode mode = Mode::Unset;

    // Liveness token shared with async done/timer callbacks. Set false when the
    // scheduler is destroyed so a stray callback (e.g. a gap timer that fires
    // during teardown) skips instead of touching freed state. NOTE: the owner
    // must still stop the worker thread that fires completions before the
    // scheduler is destroyed — this token only narrows the race, it does not
    // make concurrent destruction safe on its own.
    std::shared_ptr<std::atomic_bool> alive =
        std::make_shared<std::atomic_bool>(true);

    void updateCircuitLocked() {
        using namespace std::chrono;
        if (circuitState == CircuitState::Open) {
            auto elapsed = steady_clock::now() - circuitOpenedAt;
            if (elapsed >= milliseconds(cfg.circuitBreakerOpenMs)) {
                circuitState = CircuitState::HalfOpen;
            }
        }
    }

    void recordFailureLocked() {
        ++errorStreak;
        ++totalFailed;
        if (cfg.circuitBreakerThreshold > 0
         && errorStreak >= cfg.circuitBreakerThreshold) {
            circuitState    = CircuitState::Open;
            circuitOpenedAt = std::chrono::steady_clock::now();
        }
    }

    void recordSuccessLocked() {
        errorStreak = 0;
        ++totalCompleted;
        if (circuitState == CircuitState::HalfOpen) {
            circuitState = CircuitState::Closed;
        }
    }

    int totalQueueDepthLocked() const {
        int n = 0;
        for (auto const& lane : lanes) n += static_cast<int>(lane.size());
        return n;
    }

    // Identify a lane that has been waiting > starvation_guard_ms regardless
    // of priority. Returns -1 if no lane is starved or the guard is disabled.
    int starvedLaneLocked() const {
        if (cfg.starvationGuardMs <= 0) return -1;
        using namespace std::chrono;
        auto const now = steady_clock::now();
        auto const guard = milliseconds(cfg.starvationGuardMs);
        // Sweep low → high (we only need to bump LOW lanes when a higher
        // lane has been hogging the inflight slots).
        for (int p = 0; p < kPriorityCount; ++p) {
            auto const& lane = lanes[p];
            if (lane.empty()) continue;
            auto const oldest = lane.front()->queuedAt;
            if (now - oldest >= guard) return p;
        }
        return -1;
    }

    void pumpLocked() {
        while (inflightCount < cfg.maxInflight) {
            int picked = starvedLaneLocked();
            if (picked < 0) {
                picked = kPriorityCount;
                for (int p = kPriorityCount - 1; p >= 0; --p) {
                    if (!lanes[p].empty()) { picked = p; break; }
                }
                if (picked == kPriorityCount) return;
            }
            auto& lane = lanes[picked];
            if (lane.empty()) return;

            auto it = lane.begin();
            if (!cfg.fifoWithinLane && lane.size() > 1
             && !lastServedModule[picked].isEmpty()) {
                auto rr = std::find_if(lane.begin(), lane.end(),
                    [&](auto const& e) {
                        return e->tag.moduleId != lastServedModule[picked];
                    });
                if (rr != lane.end()) it = rr;
            }
            auto entry          = *it;
            lane.erase(it);
            lastServedModule[picked] = entry->tag.moduleId;
            entry->state             = PendingState::Selected;
            ++inflightCount;
        }
        cv.notify_all();
    }

    // When a higher-priority request lands, any lower-priority queued entry
    // tagged `interruptable=true` is preemptively cancelled so the high-pri
    // path runs sooner. Returns the number of entries cancelled.
    int preemptLowerInterruptablesLocked(Priority high) {
        int n = 0;
        for (int p = 0; p < laneIndex(high); ++p) {
            auto& lane = lanes[p];
            for (auto& e : lane) {
                if (e->state == PendingState::Queued
                 && e->tag.interruptable) {
                    e->state = PendingState::Cancelled;
                    ++n;
                    ++totalCancelled;
                }
            }
            std::erase_if(lane, [](auto const& e) {
                return e->state == PendingState::Cancelled;
            });
        }
        return n;
    }

    // ── Async (event-driven) path ──────────────────────────────────────
    // Selection mirrors pumpLocked but, instead of marking entries Selected for
    // a blocked sync waiter, it returns the async entries to invoke (outside
    // the lock) and, when the inter_request_gap is not yet satisfied, asks the
    // caller to schedule a deferred pump.
    struct PumpResult {
        std::vector<std::shared_ptr<PendingEntry>> toRun;
        int delayMs = 0;   // > 0 → caller should schedule pump(true) after this
    };

    PumpResult selectAsyncLocked() {
        PumpResult pr;
        if (stopped) return pr;   // teardown: never start more queued work
        if (delayFn && cfg.interRequestGapMs > 0
         && lastCompleteAt.time_since_epoch().count() > 0) {
            using namespace std::chrono;
            auto const elapsed = steady_clock::now() - lastCompleteAt;
            auto const gap     = milliseconds(cfg.interRequestGapMs);
            if (elapsed < gap) {
                if (!delayScheduled && totalQueueDepthLocked() > 0) {
                    delayScheduled = true;
                    pr.delayMs = int(duration_cast<milliseconds>(gap - elapsed).count()) + 1;
                }
                return pr;   // hold off until the gap elapses
            }
        }
        while (inflightCount < cfg.maxInflight) {
            int picked = starvedLaneLocked();
            if (picked < 0) {
                picked = kPriorityCount;
                for (int p = kPriorityCount - 1; p >= 0; --p) {
                    if (!lanes[p].empty()) { picked = p; break; }
                }
                if (picked == kPriorityCount) break;
            }
            auto& lane = lanes[picked];
            if (lane.empty()) break;

            auto it = lane.begin();
            if (!cfg.fifoWithinLane && lane.size() > 1
             && !lastServedModule[picked].isEmpty()) {
                auto rr = std::find_if(lane.begin(), lane.end(),
                    [&](auto const& e) {
                        return e->tag.moduleId != lastServedModule[picked];
                    });
                if (rr != lane.end()) it = rr;
            }
            auto entry          = *it;
            lane.erase(it);
            lastServedModule[picked] = entry->tag.moduleId;
            entry->state             = PendingState::Selected;
            entry->startAt           = std::chrono::steady_clock::now();
            ++inflightCount;
            pr.toRun.push_back(entry);
        }
        return pr;
    }

    void runAsync(std::vector<std::shared_ptr<PendingEntry>> const& toRun) {
        for (auto const& entry : toRun) {
            Impl* self  = this;
            auto  token = alive;   // shared_ptr copy keeps the flag alive
            AsyncDone done = [self, token, entry](bool ok) {
                if (token->load(std::memory_order_acquire)) self->completeAsync(entry, ok);
            };
            // A throwing asyncWork must not consume the in-flight slot forever:
            // complete it as a failure (idempotent if it also called done()).
            try {
                entry->asyncWork(done);
            } catch (...) {
                completeAsync(entry, false);
            }
        }
    }

    // Pump the async queue. A trampoline (pumping/repump) keeps a synchronous
    // completion — which re-enters pump() from inside runAsync — from recursing:
    // the current batch runs to completion, then the loop drains any repump.
    void pump(bool fromTimer) {
        {
            std::lock_guard lk(mtx);
            if (fromTimer) delayScheduled = false;
            if (pumping) { repump = true; return; }
            pumping = true;
        }
        for (;;) {
            PumpResult pr;
            DelayFn    delay;   // copied under the lock to avoid racing setDelayFn
            {
                std::lock_guard lk(mtx);
                pr = selectAsyncLocked();
                if (pr.delayMs > 0) delay = delayFn;
            }
            runAsync(pr.toRun);
            if (pr.delayMs > 0 && delay) {
                auto token = alive;
                delay(pr.delayMs, [this, token]() {
                    if (token->load(std::memory_order_acquire)) pump(true);
                });
            }
            std::lock_guard lk(mtx);
            if (!repump) { pumping = false; return; }
            repump = false;   // a completion arrived mid-batch — go again
        }
    }

    void completeAsync(std::shared_ptr<PendingEntry> entry, bool ok) {
        {
            std::lock_guard lk(mtx);
            if (entry->completed) return;   // ignore duplicate / double completion
            entry->completed = true;
            if (inflightCount > 0) --inflightCount;
            auto const now = std::chrono::steady_clock::now();
            lastCompleteAt = now;
            int const latency = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry->startAt).count());
            lastLatencyMs = latency;
            latencyWindow.push(latency);
            if (ok) recordSuccessLocked();
            else    recordFailureLocked();
        }
        pump(false);
    }
};

SerialScheduler::SerialScheduler(SchedulerConfig cfg)
    : m_impl(std::make_unique<Impl>(std::move(cfg))) {}

SerialScheduler::~SerialScheduler() {
    // Stop stray async callbacks (e.g. a pending gap timer) from touching freed
    // state. The owner must already have stopped the worker thread that fires
    // completions; this only guards the residual window.
    if (m_impl) m_impl->alive->store(false, std::memory_order_release);
}

SubmitResult SerialScheduler::submit(RequestTag tag,
                                      std::function<void()> work) {
    auto entry      = std::make_shared<PendingEntry>();
    entry->tag      = std::move(tag);
    entry->queuedAt = std::chrono::steady_clock::now();

    // ── Phase A: queue + wait for our turn ──────────────────────────
    {
        std::unique_lock lk(m_impl->mtx);
        if (m_impl->mode == Impl::Mode::Async) {
            return {ResultKind::Error,
                    QStringLiteral("scheduler is in async mode"), 0};
        }
        m_impl->mode = Impl::Mode::Sync;
        m_impl->updateCircuitLocked();
        if (m_impl->circuitState == CircuitState::Open) {
            return {ResultKind::CircuitOpen,
                    QStringLiteral("circuit open"), 0};
        }
        int depth = m_impl->totalQueueDepthLocked() + m_impl->inflightCount;
        if (depth >= m_impl->cfg.maxQueueDepth) {
            return {ResultKind::Error,
                    QStringLiteral("queue full"), 0};
        }
        m_impl->lanes[laneIndex(entry->tag.priority)].push_back(entry);
        ++m_impl->totalSubmitted;
        // High-priority requests preempt lower-priority interruptable peers.
        m_impl->preemptLowerInterruptablesLocked(entry->tag.priority);
        m_impl->pumpLocked();

        m_impl->cv.wait(lk, [&] {
            return entry->state == PendingState::Selected
                || entry->state == PendingState::Cancelled;
        });

        if (entry->state == PendingState::Cancelled) {
            return {ResultKind::Cancelled,
                    QStringLiteral("cancelled"), 0};
        }
    }

    // ── Phase B: enforce inter-request gap (we hold an inflight slot) ──
    using steady = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    using std::chrono::duration_cast;
    if (m_impl->cfg.interRequestGapMs > 0
     && m_impl->lastCompleteAt.time_since_epoch().count() > 0) {
        auto elapsed = steady::now() - m_impl->lastCompleteAt;
        auto gap     = milliseconds(m_impl->cfg.interRequestGapMs);
        if (elapsed < gap) {
            std::this_thread::sleep_for(gap - elapsed);
        }
    }

    // ── Phase C: run user work outside the lock ────────────────────
    auto    t0      = steady::now();
    bool    failed  = false;
    QString errMsg;
    try {
        work();
    } catch (std::exception const& e) {
        failed = true;
        errMsg = QString::fromUtf8(e.what());
    } catch (...) {
        failed = true;
        errMsg = QStringLiteral("unknown exception");
    }
    auto t1      = steady::now();
    int  latency = static_cast<int>(duration_cast<milliseconds>(t1 - t0).count());

    // ── Phase D: release & pump next ───────────────────────────────
    {
        std::lock_guard lk(m_impl->mtx);
        if (m_impl->inflightCount > 0) --m_impl->inflightCount;
        m_impl->lastCompleteAt = t1;
        m_impl->lastLatencyMs  = latency;
        m_impl->latencyWindow.push(latency);
        if (failed) m_impl->recordFailureLocked();
        else        m_impl->recordSuccessLocked();
        m_impl->pumpLocked();
    }

    if (failed) {
        return {ResultKind::Error, std::move(errMsg), latency};
    }
    return {ResultKind::Ok, {}, latency};
}

SubmitResult SerialScheduler::submitAsync(RequestTag tag, AsyncWork work) {
    {
        std::lock_guard lk(m_impl->mtx);
        if (m_impl->mode == Impl::Mode::Sync) {
            return {ResultKind::Error,
                    QStringLiteral("scheduler is in sync mode"), 0};
        }
        m_impl->mode = Impl::Mode::Async;
        m_impl->updateCircuitLocked();
        if (m_impl->circuitState == CircuitState::Open) {
            return {ResultKind::CircuitOpen, QStringLiteral("circuit open"), 0};
        }
        int const depth = m_impl->totalQueueDepthLocked() + m_impl->inflightCount;
        if (depth >= m_impl->cfg.maxQueueDepth) {
            return {ResultKind::Error, QStringLiteral("queue full"), 0};
        }
        auto entry       = std::make_shared<PendingEntry>();
        entry->tag       = std::move(tag);
        entry->queuedAt  = std::chrono::steady_clock::now();
        entry->asyncWork = std::move(work);
        m_impl->lanes[laneIndex(entry->tag.priority)].push_back(entry);
        ++m_impl->totalSubmitted;
        m_impl->preemptLowerInterruptablesLocked(entry->tag.priority);
    }
    // Pump outside the lock — the selected work runs (and may complete) without
    // the scheduler mutex held, so completion can re-enter the pump safely.
    m_impl->pump(false);
    return {ResultKind::Ok, {}, 0};
}

void SerialScheduler::setDelayFn(DelayFn fn) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->delayFn = std::move(fn);
}

void SerialScheduler::stopAsync() {
    std::lock_guard lk(m_impl->mtx);
    m_impl->stopped = true;
}

int SerialScheduler::cancelModule(QString const& moduleId) {
    int n = 0;
    {
        std::lock_guard lk(m_impl->mtx);
        for (auto& lane : m_impl->lanes) {
            for (auto& entry : lane) {
                if (entry->tag.moduleId == moduleId
                 && entry->state == PendingState::Queued) {
                    entry->state = PendingState::Cancelled;
                    ++n;
                    ++m_impl->totalCancelled;
                }
            }
            std::erase_if(lane, [](auto const& e) {
                return e->state == PendingState::Cancelled;
            });
        }
        if (n > 0) m_impl->cv.notify_all();
    }
    return n;
}

SchedulerStats SerialScheduler::stats() const {
    SchedulerStats s;
    std::lock_guard lk(m_impl->mtx);
    int total = 0;
    for (int p = 0; p < kPriorityCount; ++p) {
        s.laneQueueDepth[p] = static_cast<int>(m_impl->lanes[p].size());
        total += s.laneQueueDepth[p];
    }
    s.queueDepth         = total;
    s.inflight           = m_impl->inflightCount;
    s.maxQueueDepth      = m_impl->cfg.maxQueueDepth;
    s.totalSubmitted     = m_impl->totalSubmitted;
    s.totalCompleted     = m_impl->totalCompleted;
    s.totalFailed        = m_impl->totalFailed;
    s.totalCancelled     = m_impl->totalCancelled;
    s.totalTimedOut      = m_impl->totalTimedOut;
    s.p50LatencyMs       = m_impl->latencyWindow.percentile(0.50);
    s.p99LatencyMs       = m_impl->latencyWindow.percentile(0.99);
    s.circuitState       = m_impl->circuitState;
    s.circuitErrorStreak = m_impl->errorStreak;
    return s;
}

void SerialScheduler::recordFailureForTesting() {
    std::lock_guard lk(m_impl->mtx);
    m_impl->recordFailureLocked();
}

// ---------------------------------------------------------------------------
// Factory — dispatches on `cfg.kind`. All three flavours share the same
// SerialScheduler implementation; the kind only changes how `maxInflight` and
// `starvationGuardMs` are interpreted.
//   - Serial:   maxInflight forced to 1, no starvation guard
//   - Credit:   honours cfg.maxInflight (≥1), no starvation guard
//   - Priority: honours cfg.maxInflight (default 1), enables starvation guard
// ---------------------------------------------------------------------------
std::unique_ptr<RequestScheduler>
makeScheduler(SchedulerConfig const& cfg) {
    SchedulerConfig effective = cfg;
    switch (cfg.kind) {
        case SchedulerKind::Serial:
            effective.maxInflight       = 1;
            effective.starvationGuardMs = 0;
            break;
        case SchedulerKind::Credit:
            if (effective.maxInflight < 1) effective.maxInflight = 1;
            effective.starvationGuardMs   = 0;
            break;
        case SchedulerKind::Priority:
            if (effective.maxInflight < 1)        effective.maxInflight       = 1;
            if (effective.starvationGuardMs <= 0) effective.starvationGuardMs = 5000;
            break;
    }
    return std::make_unique<SerialScheduler>(effective);
}

} // namespace core::sched
