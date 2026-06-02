#include "core/sched/PriorityScheduler.h"

#include <utility>

namespace core::sched {

PriorityScheduler::PriorityScheduler(SchedulerConfig cfg) {
    cfg.kind = SchedulerKind::Priority;
    m_impl = makeScheduler(cfg);
}

PriorityScheduler::~PriorityScheduler() = default;

SubmitResult PriorityScheduler::submit(RequestTag tag,
                                        std::function<void()> work) {
    return m_impl->submit(std::move(tag), std::move(work));
}

int PriorityScheduler::cancelModule(QString const& moduleId) {
    return m_impl->cancelModule(moduleId);
}

SchedulerStats PriorityScheduler::stats() const {
    return m_impl->stats();
}

} // namespace core::sched
