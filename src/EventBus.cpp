// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/bus/EventBus.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core::bus {

namespace {

// One per active subscription. The handler closure lives here; the bus's
// channel table holds a weak_ptr to it, and the user-visible Subscription's
// token owns the normal strong reference. Publishers temporarily retain it
// while dispatching. Channels lazily compact expired entries.
struct Entry {
    explicit Entry(std::function<void(void const*)> h)
        : handler(std::move(h)) {}

    std::function<void(void const*)> handler;

    bool beginInvoke() {
        std::lock_guard lk(mutex);
        if (!accepting) return false;
        ++active;
        ++activeByThread[std::this_thread::get_id()];
        return true;
    }

    void endInvoke() noexcept {
        std::lock_guard lk(mutex);
        auto const tid = std::this_thread::get_id();
        if (auto it = activeByThread.find(tid); it != activeByThread.end()) {
            if (--it->second == 0) activeByThread.erase(it);
        }
        if (active > 0) --active;
        drained.notify_all();
    }

    bool invoke(void const* event) {
        if (!beginInvoke()) return false;
        try {
            handler(event);
            endInvoke();
            return true;
        } catch (...) {
            endInvoke();
            throw;
        }
    }

    void cancelAndDrain() noexcept {
        std::unique_lock lk(mutex);
        accepting = false;
        // A handler may destroy/cancel its own Subscription. Waiting for the
        // current invocation would deadlock, so drain every *other* thread and
        // let this stack frame finish while its publisher retains the Entry.
        auto const it = activeByThread.find(std::this_thread::get_id());
        int const selfActive = it == activeByThread.end() ? 0 : it->second;
        drained.wait(lk, [&] { return active <= selfActive; });
    }

    std::mutex                              mutex;
    std::condition_variable                 drained;
    bool                                    accepting = true;
    int                                     active    = 0;
    std::unordered_map<std::thread::id, int> activeByThread;
};

struct SubscriptionToken {
    std::shared_ptr<Entry> entry;
    ~SubscriptionToken() { if (entry) entry->cancelAndDrain(); }
};

} // namespace

class EventBus::Impl {
public:
    mutable std::mutex                                                       mutex;
    std::unordered_map<std::type_index, std::vector<std::weak_ptr<Entry>>>   channels;
    std::atomic<quint64>                                                     totalPublished{0};
    std::atomic<quint64>                                                     totalDelivered{0};
    std::atomic<quint64>                                                     totalHandlerFailures{0};
};

EventBus::EventBus(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>()) {}

EventBus::~EventBus() = default;

Subscription EventBus::subscribeErased(
    std::type_index                  ti,
    std::function<void(void const*)> handler) {
    auto entry = std::make_shared<Entry>(std::move(handler));
    {
        std::lock_guard lk(m_impl->mutex);
        auto& list = m_impl->channels[ti];
        std::erase_if(list, [](std::weak_ptr<Entry> const& w) { return w.expired(); });
        list.emplace_back(entry);
    }
    auto token = std::make_shared<SubscriptionToken>();
    token->entry = std::move(entry);
    return Subscription(std::shared_ptr<void>(std::move(token)));
}

void EventBus::publishErased(std::type_index ti, void const* event) {
    m_impl->totalPublished.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::shared_ptr<Entry>> alive;
    {
        std::lock_guard lk(m_impl->mutex);
        auto it = m_impl->channels.find(ti);
        if (it == m_impl->channels.end()) {
            return;
        }
        auto& list = it->second;
        alive.reserve(list.size());
        std::erase_if(list, [&alive](std::weak_ptr<Entry>& w) {
            if (auto sp = w.lock()) {
                alive.push_back(std::move(sp));
                return false;
            }
            return true;
        });
    }
    // Dispatch outside the mutex so handlers may freely subscribe, publish,
    // or destroy other subscriptions reentrantly without deadlocking.
    for (auto const& sp : alive) {
        try {
            if (sp->invoke(event)) {
                m_impl->totalDelivered.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            m_impl->totalHandlerFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

BusStats EventBus::stats() const {
    BusStats s;
    s.totalPublished = m_impl->totalPublished.load(std::memory_order_relaxed);
    s.totalDelivered = m_impl->totalDelivered.load(std::memory_order_relaxed);
    s.totalHandlerFailures =
        m_impl->totalHandlerFailures.load(std::memory_order_relaxed);
    {
        std::lock_guard lk(m_impl->mutex);
        int count = 0;
        for (auto const& [_, list] : m_impl->channels) {
            for (auto const& w : list) {
                if (!w.expired()) ++count;
            }
        }
        s.activeSubscribers = count;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

Subscription::Subscription(std::shared_ptr<void> handle) noexcept
    : m_handle(std::move(handle)) {}

Subscription::~Subscription()                                       = default;
Subscription::Subscription(Subscription&&) noexcept                 = default;
Subscription& Subscription::operator=(Subscription&&) noexcept      = default;

void Subscription::cancel()        noexcept { m_handle.reset(); }
bool Subscription::active() const  noexcept { return static_cast<bool>(m_handle); }

} // namespace core::bus
