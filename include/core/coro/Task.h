#pragma once

#include <coroutine>
#include <exception>

#include "core/core_global.h"

namespace core::coro {

// Fire-and-forget coroutine return type. Unlike Lazy<T>, Task starts running
// eagerly at the point of construction and owns its own lifetime through the
// promise's `final_suspend()` returning suspend_never (the coroutine frame
// is destroyed automatically when control flow reaches the end).
//
// Use Task for background work that does not need to be awaited (timers,
// fan-out emitters, side-effecting subscriptions). Use Lazy<T> when callers
// need the result.
struct Task {
    struct promise_type {
        Task get_return_object() noexcept                { return Task{}; }
        std::suspend_never initial_suspend() noexcept    { return {}; }
        std::suspend_never final_suspend()   noexcept    { return {}; }
        void               return_void()     noexcept    {}
        void               unhandled_exception() noexcept;
    };
};

} // namespace core::coro
