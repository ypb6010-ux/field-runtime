// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
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
// read / writeBatch are synchronous compatibility entry points. Concrete
// implementations either complete inline or block the caller while their
// device-thread operation finishes. Runtime modules use readAsync/writeAsync
// through the scheduler so device waits do not park the calling thread.
class CORE_EXPORT Transport {
public:
    virtual ~Transport() = default;

    virtual QString          id()    const = 0;
    virtual TransportKind    kind()  const = 0;
    virtual ConnectionState  state() const = 0;

    // Thread-safe value snapshots for initialization, diagnostics and tests.
    // Concrete transports override status() to include endpoints, errors and
    // monotonic revisions. The default keeps lightweight test transports
    // source-compatible.
    virtual TransportStatus status() const {
        TransportStatus out;
        out.transportId = id();
        out.kind        = kind();
        out.state       = state();
        return out;
    }
    virtual QList<PeerSession> peerSessions() const { return {}; }

    virtual std::expected<void, QString> connect()    = 0;
    virtual void                          disconnect() = 0;

    virtual sched::RequestScheduler& scheduler() = 0;

    // Synchronous compatibility API. Do not call these from the transport's
    // own worker thread; use readAsync/writeAsync there.
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
