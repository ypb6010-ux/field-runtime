#pragma once

#include "core/core_global.h"
#include "core/log/LogTypes.h"

namespace core::log {

// A log destination. Implementations receive records on the Logger's single
// dispatch thread, so write()/flush() need not be reentrant against each other
// — but must not block for long, since one slow sink delays the rest.
//
// Encapsulation contract: sinks only ever RECEIVE value records. Registering a
// sink is the owner's (ICore / composition root) privilege, never a plugin's
// or the UI's — those only emit.
class CORE_EXPORT ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void write(LogRecord const& rec)       = 0;
    virtual void write(OperationRecord const& rec) = 0;
    virtual void flush() {}
};

} // namespace core::log
