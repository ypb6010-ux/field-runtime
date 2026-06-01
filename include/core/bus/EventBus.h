#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <utility>

#include <QObject>

#include "core/core_global.h"
#include "core/bus/Subscription.h"
#include "core/coro/Lazy.h"

namespace core::bus {

struct BusStats {
    quint64 totalPublished    = 0;
    quint64 totalDelivered    = 0;
    int     activeSubscribers = 0;
};

// EventBus is owned by Core; lifetime ends with Core. Publish is thread-safe
// and may be called from any thread; in the Phase 1 implementation handlers
// run synchronously on the publisher's thread (the worker-thread dispatch
// design from spec §2.1 is a later optimization — the public API does not
// change).
class CORE_EXPORT EventBus : public QObject {
    Q_OBJECT
public:
    explicit EventBus(QObject* parent = nullptr);
    ~EventBus() override;

    CORE_DISABLE_COPY_MOVE(EventBus)

    template <class T>
    Subscription subscribe(std::function<void(T const&)> handler) {
        std::function<void(void const*)> erased =
            [h = std::move(handler)](void const* p) {
                h(*static_cast<T const*>(p));
            };
        return subscribeErased(std::type_index(typeid(T)), std::move(erased));
    }

    template <class T>
    void publish(T const& event) {
        publishErased(std::type_index(typeid(T)),
                      static_cast<void const*>(&event));
    }

    // TODO(phase-2): coroutine wait with timeout, used by AckWatch. The
    // signature is reserved here so consumers can compile-time discover the
    // intent; calling it before the implementation lands triggers a linker
    // error rather than silent misbehaviour.
    template <class T>
    coro::Lazy<std::optional<T>> waitFor(
        std::function<bool(T const&)> predicate,
        int timeoutMs);

    BusStats stats() const;

private:
    Subscription subscribeErased(std::type_index                 ti,
                                  std::function<void(void const*)> handler);
    void         publishErased  (std::type_index                 ti,
                                  void const*                      event);

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::bus
