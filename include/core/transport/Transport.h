#pragma once

#include <QString>
#include <expected>

#include "core/core_global.h"
#include "core/transport/TransportTypes.h"
#include "core/sched/RequestScheduler.h"
#include "core/coro/Lazy.h"

namespace core::transport {

// Abstract transport. Owns its scheduler. All reads/writes from external code
// MUST go through `scheduler().submit(...)`; direct calls to read/writeBatch
// are reserved for the scheduler internal pump.
class CORE_EXPORT Transport {
public:
    virtual ~Transport() = default;

    virtual QString          id() const   = 0;
    virtual TransportKind    kind() const = 0;
    virtual ConnectionState  state() const = 0;

    virtual std::expected<void, QString> connect()    = 0;
    virtual void                          disconnect() = 0;

    virtual sched::RequestScheduler& scheduler() = 0;

    // Internal — invoked by the scheduler only.
    virtual coro::Lazy<ReadResult>  read(ReadRequest const& req)       = 0;
    virtual coro::Lazy<WriteResult> writeBatch(WriteBatch const& batch) = 0;
};

} // namespace core::transport
