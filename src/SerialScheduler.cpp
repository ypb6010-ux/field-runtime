#include "core/sched/SerialScheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

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
};

SerialScheduler::SerialScheduler(SchedulerConfig cfg)
    : m_impl(std::make_unique<Impl>(std::move(cfg))) {}

SerialScheduler::~SerialScheduler() = default;

SubmitResult SerialScheduler::submit(RequestTag tag,
                                      std::function<void()> work) {
    auto entry      = std::make_shared<PendingEntry>();
    entry->tag      = std::move(tag);
    entry->queuedAt = std::chrono::steady_clock::now();

    // ── Phase A: queue + wait for our turn ──────────────────────────
    {
        std::unique_lock lk(m_impl->mtx);
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
