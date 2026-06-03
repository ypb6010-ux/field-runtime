#include "core/sched/CreditScheduler.h"

#include <utility>

namespace core::sched {

CreditScheduler::CreditScheduler(SchedulerConfig cfg) {
    cfg.kind = SchedulerKind::Credit;
    m_impl = makeScheduler(cfg);
}

CreditScheduler::~CreditScheduler() = default;

SubmitResult CreditScheduler::submit(RequestTag tag,
                                      std::function<void()> work) {
    return m_impl->submit(std::move(tag), std::move(work));
}

SubmitResult CreditScheduler::submitAsync(RequestTag tag, AsyncWork work) {
    return m_impl->submitAsync(std::move(tag), std::move(work));
}

void CreditScheduler::setDelayFn(DelayFn fn) {
    m_impl->setDelayFn(std::move(fn));
}

int CreditScheduler::cancelModule(QString const& moduleId) {
    return m_impl->cancelModule(moduleId);
}

SchedulerStats CreditScheduler::stats() const {
    return m_impl->stats();
}

} // namespace core::sched
