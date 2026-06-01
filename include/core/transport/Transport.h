#pragma once

#include <QString>
#include <expected>

#include "core/core_global.h"
#include "core/transport/TransportTypes.h"
#include "core/sched/RequestScheduler.h"

namespace core::transport {

// Abstract transport. Owns its scheduler. All reads/writes from external code
// MUST go through `scheduler().submit(...)`; direct calls to `read` /
// `writeBatch` are reserved for the scheduler's pump.
//
// Phase 1 contract: read / writeBatch are synchronous. Concrete
// implementations either complete the I/O inline (mock, test fixtures) or
// block the calling thread waiting for the underlying QModbusReply / socket.
// Because every caller routes through `scheduler().submit(work)` with a
// `std::function<void()>`, the scheduler thread serialises requests for
// half-duplex devices regardless of whether the I/O itself is sync or async.
//
// Phase 2 will add `coro::Lazy<ReadResult> readAsync(...)` overloads so
// upper-layer coroutines (PollRange, SinkWindow, AckWatch) can be expressed
// without blocking a thread per pending request.
class CORE_EXPORT Transport {
public:
    virtual ~Transport() = default;

    virtual QString          id()    const = 0;
    virtual TransportKind    kind()  const = 0;
    virtual ConnectionState  state() const = 0;

    virtual std::expected<void, QString> connect()    = 0;
    virtual void                          disconnect() = 0;

    virtual sched::RequestScheduler& scheduler() = 0;

    // Internal — called from inside scheduler.submit work lambdas, never
    // from arbitrary user code.
    virtual ReadResult  read      (ReadRequest const& req)         = 0;
    virtual WriteResult writeBatch(WriteBatch  const& batch)       = 0;
};

} // namespace core::transport
