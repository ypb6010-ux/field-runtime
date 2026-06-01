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

    virtual int            cancelModule(QString const& moduleId) = 0;
    virtual SchedulerStats stats() const                         = 0;
};

CORE_EXPORT std::unique_ptr<RequestScheduler>
            makeScheduler(SchedulerConfig const& cfg);

} // namespace core::sched
