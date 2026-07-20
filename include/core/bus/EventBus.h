// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <typeindex>
#include <utility>

#include <QObject>

#include "core/core_global.h"
#include "core/bus/Subscription.h"

namespace core::bus {

struct BusStats {
    quint64 totalPublished    = 0;
    quint64 totalDelivered    = 0;
    quint64 totalHandlerFailures = 0;
    int     activeSubscribers = 0;
};

// EventBus is owned by Core; lifetime ends with Core. Publish is thread-safe
// and may be called from any thread. Handlers run synchronously on the
// publisher's thread; one throwing handler is isolated and does not prevent
// later subscribers from receiving the event.
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
