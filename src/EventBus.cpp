#include "core/bus/EventBus.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core::bus {

namespace {

// One per active subscription. The handler closure lives here; the bus's
// channel table holds a weak_ptr to it, and the user-visible Subscription
// holds the only shared_ptr. When the Subscription dies the entry is
// destroyed and the weak_ptr expires — channels lazily skip and compact
// expired entries on the next publish or subscribe.
struct Entry {
    std::function<void(void const*)> handler;
};

} // namespace

class EventBus::Impl {
public:
    mutable std::mutex                                                       mutex;
    std::unordered_map<std::type_index, std::vector<std::weak_ptr<Entry>>>   channels;
    std::atomic<quint64>                                                     totalPublished{0};
    std::atomic<quint64>                                                     totalDelivered{0};
};

EventBus::EventBus(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>()) {}

EventBus::~EventBus() = default;

Subscription EventBus::subscribeErased(
    std::type_index                  ti,
    std::function<void(void const*)> handler) {
    auto entry = std::make_shared<Entry>(Entry{std::move(handler)});
    {
        std::lock_guard lk(m_impl->mutex);
        auto& list = m_impl->channels[ti];
        std::erase_if(list, [](std::weak_ptr<Entry> const& w) { return w.expired(); });
        list.emplace_back(entry);
    }
    return Subscription(std::shared_ptr<void>(std::move(entry)));
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
        sp->handler(event);
        m_impl->totalDelivered.fetch_add(1, std::memory_order_relaxed);
    }
}

BusStats EventBus::stats() const {
    BusStats s;
    s.totalPublished = m_impl->totalPublished.load(std::memory_order_relaxed);
    s.totalDelivered = m_impl->totalDelivered.load(std::memory_order_relaxed);
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
