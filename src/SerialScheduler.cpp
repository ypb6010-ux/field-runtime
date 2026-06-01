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
    RequestTag    tag;
    PendingState  state = PendingState::Queued;
};

constexpr int laneIndex(Priority p) noexcept { return static_cast<int>(p); }

} // namespace

class SerialScheduler::Impl {
public:
    explicit Impl(SchedulerConfig c) : cfg(std::move(c)) {}

    SchedulerConfig                                                   cfg;
    mutable std::mutex                                                mtx;
    std::condition_variable                                           cv;
    std::array<std::deque<std::shared_ptr<PendingEntry>>, kPriorityCount> lanes;
    std::array<QString, kPriorityCount>                               lastServedModule;
    bool                                                              inflight = false;
    std::chrono::steady_clock::time_point                             lastCompleteAt;

    quint64 totalSubmitted = 0;
    quint64 totalCompleted = 0;
    quint64 totalFailed    = 0;
    quint64 totalCancelled = 0;
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

    void pumpLocked() {
        if (inflight) return;
        for (int p = kPriorityCount - 1; p >= 0; --p) {
            auto& lane = lanes[p];
            if (lane.empty()) continue;

            auto it = lane.begin();
            if (!cfg.fifoWithinLane && lane.size() > 1
             && !lastServedModule[p].isEmpty()) {
                auto rr = std::find_if(lane.begin(), lane.end(),
                    [&](auto const& e) {
                        return e->tag.moduleId != lastServedModule[p];
                    });
                if (rr != lane.end()) it = rr;
            }
            auto entry          = *it;
            lane.erase(it);
            lastServedModule[p] = entry->tag.moduleId;
            entry->state        = PendingState::Selected;
            inflight            = true;
            cv.notify_all();
            return;
        }
    }
};

SerialScheduler::SerialScheduler(SchedulerConfig cfg)
    : m_impl(std::make_unique<Impl>(std::move(cfg))) {}

SerialScheduler::~SerialScheduler() = default;

SubmitResult SerialScheduler::submit(RequestTag tag,
                                      std::function<void()> work) {
    auto entry = std::make_shared<PendingEntry>();
    entry->tag = std::move(tag);

    // ── Phase A: queue + wait for our turn ──────────────────────────
    {
        std::unique_lock lk(m_impl->mtx);
        m_impl->updateCircuitLocked();
        if (m_impl->circuitState == CircuitState::Open) {
            return {ResultKind::CircuitOpen,
                    QStringLiteral("circuit open"), 0};
        }
        int depth = m_impl->totalQueueDepthLocked()
                  + (m_impl->inflight ? 1 : 0);
        if (depth >= m_impl->cfg.maxQueueDepth) {
            return {ResultKind::Error,
                    QStringLiteral("queue full"), 0};
        }
        m_impl->lanes[laneIndex(entry->tag.priority)].push_back(entry);
        ++m_impl->totalSubmitted;
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

    // ── Phase B: enforce inter-request gap (we hold the inflight slot) ──
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
        m_impl->inflight       = false;
        m_impl->lastCompleteAt = t1;
        m_impl->lastLatencyMs  = latency;
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
    s.inflight           = m_impl->inflight ? 1 : 0;
    s.maxQueueDepth      = m_impl->cfg.maxQueueDepth;
    s.totalSubmitted     = m_impl->totalSubmitted;
    s.totalCompleted     = m_impl->totalCompleted;
    s.totalFailed        = m_impl->totalFailed;
    s.totalCancelled     = m_impl->totalCancelled;
    s.totalTimedOut      = 0;                 // Phase 3
    s.p50LatencyMs       = m_impl->lastLatencyMs;   // crude until Phase 3
    s.p99LatencyMs       = m_impl->lastLatencyMs;
    s.circuitState       = m_impl->circuitState;
    s.circuitErrorStreak = m_impl->errorStreak;
    return s;
}

void SerialScheduler::recordFailureForTesting() {
    std::lock_guard lk(m_impl->mtx);
    m_impl->recordFailureLocked();
}

// ---------------------------------------------------------------------------
// Factory — only SerialScheduler is implemented in Phase 1; Credit / Priority
// strategies are planned for Phase 3. Until they land, any kind falls back to
// Serial so call sites can already pin a SchedulerKind in their config.
// ---------------------------------------------------------------------------
std::unique_ptr<RequestScheduler>
makeScheduler(SchedulerConfig const& cfg) {
    return std::make_unique<SerialScheduler>(cfg);
}

} // namespace core::sched
