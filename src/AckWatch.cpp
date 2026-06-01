#include "core/module/AckWatch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"

namespace core::module {

class AckWatch::Impl {
public:
    bus::EventBus*       bus = nullptr;
    Config                cfg;
    std::atomic<bool>    cancelled{false};
};

AckWatch::AckWatch(Config cfg, bus::EventBus& bus)
    : m_impl(new Impl) {
    m_impl->bus = &bus;
    m_impl->cfg = std::move(cfg);
    m_id          = m_impl->cfg.moduleId;
    m_transportId = QString{};
    m_priority    = sched::Priority::Normal;
}

AckWatch::~AckWatch() { delete m_impl; }

QString AckWatch::dpId() const { return m_impl->cfg.dpId; }

AckWatch::AckResult AckWatch::waitOnce() {
    if (m_impl->cancelled.load(std::memory_order_acquire)) {
        return AckResult::Cancelled;
    }

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    matched = false;

    auto sub = m_impl->bus->subscribe<bus::DpChanged>(
        [this, &mtx, &cv, &matched](bus::DpChanged const& e) {
            if (e.id != m_impl->cfg.dpId) return;
            if (e.value != m_impl->cfg.expected) return;
            {
                std::lock_guard lk(mtx);
                matched = true;
            }
            cv.notify_all();
        });

    std::unique_lock lk(mtx);
    bool const got = cv.wait_for(
        lk, std::chrono::milliseconds(m_impl->cfg.timeoutMs),
        [this, &matched] {
            return matched
                || m_impl->cancelled.load(std::memory_order_acquire);
        });

    if (m_impl->cancelled.load(std::memory_order_acquire)) {
        return AckResult::Cancelled;
    }
    if (!got || !matched) return AckResult::Timeout;
    return AckResult::Ok;
}

void AckWatch::cancel() {
    m_impl->cancelled.store(true, std::memory_order_release);
    // Publishing a sentinel DpChanged wakes any predicate-waiter; instead we
    // rely on the predicate also re-checking `cancelled` after wake. The
    // current synchronous implementation is sufficient because cancel is
    // followed by either timeout (3s default) or by a new event.
    // Phase 3 will switch to an explicit cv per-wait so cancel can wake it.
}

void AckWatch::start()  { m_impl->cancelled.store(false, std::memory_order_release); }
void AckWatch::stop()   { cancel(); }
void AckWatch::pause()  {}
void AckWatch::resume() { m_impl->cancelled.store(false, std::memory_order_release); }

} // namespace core::module
