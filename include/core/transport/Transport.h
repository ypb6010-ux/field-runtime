// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <expected>
#include <functional>
#include <string>
#include <vector>

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

    virtual std::string      id()    const = 0;
    virtual TransportKind    kind()  const = 0;
    virtual ConnectionState  state() const = 0;
    virtual TransportStatus status() const {
        TransportStatus snapshot;
        snapshot.transportId = id();
        snapshot.kind = kind();
        snapshot.state = state();
        return snapshot;
    }
    virtual std::vector<PeerSession> peerSessions() const { return {}; }

    virtual std::expected<void, std::string> connect()    = 0;
    virtual void                             disconnect() = 0;

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

    // Operator-initiated reconnect (e.g. control-socket `reconnect`/`reset`).
    // Must be non-blocking and must NOT tear down the scheduler. The default is
    // a no-op for transports without a self-healing reconnect path; transports
    // that have one (e.g. AsioModbusTcpClient) override it to re-arm async.
    virtual void requestReconnect() {}
};

} // namespace core::transport
