// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/sched/SchedulerTypes.h"

namespace core::sched {

struct SubmitResult {
    ResultKind kind         = ResultKind::Ok;
    QString    errorMessage;
    int        latencyMs    = 0;
};

// Event-driven submit types. `AsyncWork` initiates non-blocking I/O and MUST
// call its `AsyncDone` exactly once when the I/O finishes, reporting whether it
// succeeded (for the circuit breaker / stats). `DelayFn` lets the scheduler
// defer its next pump by `ms` without blocking — the owning transport supplies
// one backed by a timer on its worker thread (used to honour inter_request_gap
// in the async path). When no DelayFn is set, the gap is not enforced.
using AsyncDone = std::function<void(bool /*ok*/)>;
using AsyncWork = std::function<void(AsyncDone)>;
using DelayFn   = std::function<void(int /*ms*/, std::function<void()>)>;

// Request gatekeeper attached to a Transport. submit() is the synchronous
// compatibility path; submitAsync() is the runtime path and serialises device
// work without parking a caller thread. Device I/O timeouts belong to the
// Transport's request_timeout_ms setting.
class CORE_EXPORT RequestScheduler {
public:
    virtual ~RequestScheduler() = default;

    virtual SubmitResult submit(RequestTag tag,
                                std::function<void()> work) = 0;

    // Event-driven submit. Returns immediately: kind == Ok means the work was
    // accepted and its AsyncDone will run when the I/O completes; CircuitOpen /
    // Error(queue full) mean it was rejected and the work will NOT run.
    // Non-blocking — no caller thread is parked.
    virtual SubmitResult submitAsync(RequestTag tag, AsyncWork work) = 0;

    // Install the deferred-pump hook used to honour inter_request_gap in the
    // async path (see DelayFn). Optional; called once at wiring time.
    virtual void setDelayFn(DelayFn fn) = 0;

    // Halt the async path (teardown): no more queued submitAsync work is
    // selected/run; an in-flight op still completes. The owning transport calls
    // this before joining its worker thread so a completion firing during
    // teardown cannot pump the next request into a half-destroyed transport.
    virtual void stopAsync() = 0;

    // Cancel queued synchronous submissions for a module. Accepted async work
    // is deliberately not discarded: submitAsync promises a completion, and
    // silently dropping it would leave module coalescing guards permanently
    // in-flight.
    virtual int            cancelModule(QString const& moduleId) = 0;
    virtual SchedulerStats stats() const                         = 0;
};

CORE_EXPORT std::unique_ptr<RequestScheduler>
            makeScheduler(SchedulerConfig const& cfg);

} // namespace core::sched
