#pragma once

#include <memory>

#include "core/core_global.h"
#include "core/sched/SchedulerTypes.h"
#include "core/coro/Lazy.h"

namespace core::sched {

class CORE_EXPORT RequestScheduler {
public:
    virtual ~RequestScheduler() = default;

    // Submit a unit of work. The scheduler decides when to execute it based on
    // the configured strategy (serial/credit/priority). Returns the underlying
    // work's result asynchronously.
    //
    // Implementation note: the template form is type-erased into a concrete
    // submit on a `std::function<Lazy<SubmitResult>()>` carrier; the result
    // type T is preserved via a thin coroutine wrapper. Defined out-of-line
    // alongside subclasses to keep this header minimal.
    template <class F>
    auto submit(RequestTag tag, F&& work);

    virtual int            cancelModule(QString const& moduleId) = 0;
    virtual SchedulerStats stats() const                         = 0;
};

CORE_EXPORT std::unique_ptr<RequestScheduler> makeScheduler(SchedulerConfig const& cfg);

} // namespace core::sched
