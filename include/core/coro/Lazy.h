// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Lazy<T> is the project-wide single-result coroutine type. It is provided by
// async_simple (https://github.com/alibaba/async_simple) rather than handwritten
// here, because async_simple is already pinned in vcpkg.json and provides a
// mature scheduler integration plus collectAll / collectAllPara helpers that
// Core needs from day one.
//
// We re-export it inside core::coro so that callers depend on the project's
// own namespace and we retain the ability to swap implementations later
// without touching every call site.

#include <async_simple/coro/Lazy.h>

#include "core/core_global.h"

namespace core::coro {

template <class T>
using Lazy = async_simple::coro::Lazy<T>;

using async_simple::Try;

} // namespace core::coro
