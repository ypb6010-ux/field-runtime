#pragma once

#include <functional>
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

    // Non-blocking I/O. The request is dispatched to the transport's worker
    // thread and `done(result)` is invoked there when the reply finishes — no
    // caller thread ever blocks waiting on the device. This is what lets the
    // event-driven scheduler run a poll without parking a thread per request.
    //
    // The default implementation just runs the synchronous read/writeBatch on
    // the calling thread and forwards the result — correct but blocking, for
    // transports not yet converted (and for MockTransport, whose I/O is
    // instantaneous). Real device transports override with a true async path.
    using ReadDone  = std::function<void(ReadResult)>;
    using WriteDone = std::function<void(WriteResult)>;
    virtual void readAsync (ReadRequest const& req,   ReadDone  done) {
        done(read(req));
    }
    virtual void writeAsync(WriteBatch  const& batch, WriteDone done) {
        done(writeBatch(batch));
    }
};

} // namespace core::transport
