// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/core_global.h"

namespace core::bus {

// RAII handle for an EventBus subscription. Destruction/cancel prevents new
// invocations and drains callbacks already running on other threads before it
// returns. Cancelling from inside the handler itself is supported.
class CORE_EXPORT Subscription {
public:
    Subscription() noexcept = default;
    ~Subscription();

    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;

    Subscription(Subscription const&)            = delete;
    Subscription& operator=(Subscription const&) = delete;

    void cancel() noexcept;
    bool active() const noexcept;

private:
    friend class EventBus;
    explicit Subscription(std::shared_ptr<void> handle) noexcept;
    std::shared_ptr<void> m_handle;
};

} // namespace core::bus
