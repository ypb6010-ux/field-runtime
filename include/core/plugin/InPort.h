#pragma once

#include <functional>

#include "core/core_global.h"

namespace core::plugin {

// Typed input channel into a plugin from a datapoint. Bound at startup via
// PortRegistry::bindIn; values arrive on the EventBus dispatch thread.
template <class T>
class InPort {
public:
    using Handler = std::function<void(T const&)>;

    void onChanged(Handler h) { m_handler = std::move(h); }

    // Invoked by Core/PortRegistry — not user-facing.
    void deliver(T const& v) { if (m_handler) m_handler(v); }

private:
    Handler m_handler;
};

} // namespace core::plugin
