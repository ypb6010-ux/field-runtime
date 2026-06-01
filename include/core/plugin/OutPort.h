#pragma once

#include <functional>

#include "core/core_global.h"

namespace core::plugin {

// Typed output channel from a plugin into a Command/Bidirectional datapoint.
template <class T>
class OutPort {
public:
    using Emitter = std::function<void(T const&)>;

    void bindEmitter(Emitter e) { m_emit = std::move(e); }

    // Named `send` (not `emit`) to avoid clashing with the Qt `emit` macro
    // which expands to empty when any QtCore header has been included.
    void send(T const& value) { if (m_emit) m_emit(value); }

private:
    Emitter m_emit;
};

} // namespace core::plugin
