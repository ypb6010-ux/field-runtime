#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <typeindex>

#include <QObject>

#include "core/core_global.h"
#include "core/bus/Subscription.h"
#include "core/coro/Lazy.h"

namespace core::bus {

struct BusStats {
    quint64 totalPublished  = 0;
    quint64 totalDelivered  = 0;
    int     activeSubscribers = 0;
};

// EventBus is owned by Core; lifetime ends with Core. Publish is thread-safe
// and may be called from any thread; dispatch happens on the bus's internal
// worker thread and delivers handlers serially per channel.
class CORE_EXPORT EventBus : public QObject {
    Q_OBJECT
public:
    explicit EventBus(QObject* parent = nullptr);
    ~EventBus() override;

    CORE_DISABLE_COPY_MOVE(EventBus)

    template <class T>
    Subscription subscribe(std::function<void(T const&)> handler);

    template <class T>
    void publish(T const& event);

    template <class T>
    coro::Lazy<std::optional<T>> waitFor(
        std::function<bool(T const&)> predicate,
        int timeoutMs);

    BusStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::bus
