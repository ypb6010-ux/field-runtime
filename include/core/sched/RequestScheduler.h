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

// RequestScheduler — abstract base for the request gatekeeper attached to a
// Transport. The Phase 1 contract is intentionally simple: callers submit a
// synchronous `std::function<void()>` and the scheduler decides when to
// actually invoke it based on the configured strategy (serial / credit /
// priority). The caller's thread blocks inside `submit` until either the
// work runs to completion or it is cancelled / circuit-opened / timed out.
//
// Phase 2 will introduce a coroutine variant returning `Lazy<R>` so callers
// in their own coroutines do not block a worker thread; the underlying
// queue / priority lane / cancellation logic carries over unchanged.
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

    virtual int            cancelModule(QString const& moduleId) = 0;
    virtual SchedulerStats stats() const                         = 0;
};

CORE_EXPORT std::unique_ptr<RequestScheduler>
            makeScheduler(SchedulerConfig const& cfg);

} // namespace core::sched
