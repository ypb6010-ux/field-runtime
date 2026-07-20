// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/module/AckWatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"

namespace core::module {

class AckWatch::Impl {
public:
    struct Waiter {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    matched = false;
    };

    bus::EventBus*       bus = nullptr;
    dp::DatapointRegistry* datapoints = nullptr;
    Config                cfg;
    std::atomic<bool>    cancelled{false};
    std::mutex           waitersMtx;
    std::condition_variable drainedCv;
    std::vector<std::weak_ptr<Waiter>> waiters;
    int                  activeWaits = 0;
};

AckWatch::AckWatch(Config cfg, bus::EventBus& bus)
    : AckWatch(std::move(cfg), bus, nullptr) {}

AckWatch::AckWatch(Config cfg, bus::EventBus& bus,
                   dp::DatapointRegistry* datapoints)
    : m_impl(new Impl) {
    m_impl->bus = &bus;
    m_impl->datapoints = datapoints;
    m_impl->cfg = std::move(cfg);
    m_id          = m_impl->cfg.moduleId;
    m_transportId = QString{};
    m_priority    = sched::Priority::Normal;
}

AckWatch::~AckWatch() {
    cancel();
    {
        std::unique_lock lk(m_impl->waitersMtx);
        m_impl->drainedCv.wait(lk, [this] { return m_impl->activeWaits == 0; });
    }
    delete m_impl;
}

QString AckWatch::dpId() const { return m_impl->cfg.dpId; }

AckWatch::AckResult AckWatch::waitOnce() {
    if (m_impl->cancelled.load(std::memory_order_acquire)) {
        return AckResult::Cancelled;
    }

    auto waiter = std::make_shared<Impl::Waiter>();
    {
        std::lock_guard lk(m_impl->waitersMtx);
        ++m_impl->activeWaits;
        m_impl->waiters.emplace_back(waiter);
    }

    // Capture immutable match criteria and shared waiter state, never this or
    // stack references. EventBus may already hold a dispatch snapshot when the
    // subscription is destroyed after timeout.
    QString const dpId = m_impl->cfg.dpId;
    QVariant const expected = m_impl->cfg.expected;

    auto sub = m_impl->bus->subscribe<bus::DpChanged>(
        [waiter, dpId, expected](bus::DpChanged const& e) {
            if (e.id != dpId || e.value != expected) return;
            {
                std::lock_guard lk(waiter->mtx);
                waiter->matched = true;
            }
            waiter->cv.notify_all();
        });

    // Subscribe first, then inspect the current datapoint. An acknowledgement
    // that arrived before waitOnce is observed from the registry; one racing
    // this check is observed either by the subscription or by the value read.
    if (m_impl->datapoints) {
        if (auto dp = m_impl->datapoints->find(dpId);
            dp && dp->valid() && dp->value() == expected) {
            {
                std::lock_guard lk(waiter->mtx);
                waiter->matched = true;
            }
            waiter->cv.notify_all();
        }
    }

    std::unique_lock lk(waiter->mtx);
    bool const got = waiter->cv.wait_for(
        lk, std::chrono::milliseconds(m_impl->cfg.timeoutMs),
        [this, &waiter] {
            return waiter->matched
                || m_impl->cancelled.load(std::memory_order_acquire);
        });
    bool const matched = waiter->matched;
    lk.unlock();
    bool const wasCancelled = m_impl->cancelled.load(std::memory_order_acquire);

    {
        std::lock_guard waitersLock(m_impl->waitersMtx);
        --m_impl->activeWaits;
        std::erase_if(m_impl->waiters,
            [](std::weak_ptr<Impl::Waiter> const& w) { return w.expired(); });
        if (m_impl->activeWaits == 0) m_impl->drainedCv.notify_all();
    }

    // No m_impl access after activeWaits is decremented: the destructor may
    // have been waiting for this call and can now release the implementation.
    if (wasCancelled) {
        return AckResult::Cancelled;
    }
    if (!got || !matched) return AckResult::Timeout;
    return AckResult::Ok;
}

void AckWatch::cancel() {
    m_impl->cancelled.store(true, std::memory_order_release);
    std::lock_guard lk(m_impl->waitersMtx);
    for (auto const& weak : m_impl->waiters) {
        if (auto waiter = weak.lock()) waiter->cv.notify_all();
    }
}

void AckWatch::start()  { m_impl->cancelled.store(false, std::memory_order_release); }
void AckWatch::stop()   { cancel(); }
void AckWatch::pause()  { cancel(); }
void AckWatch::resume() { m_impl->cancelled.store(false, std::memory_order_release); }

} // namespace core::module
