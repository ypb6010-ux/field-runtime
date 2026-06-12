// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/core_global.h"
#include "core/sched/RequestScheduler.h"

namespace core::sched {

// PriorityScheduler — strict-priority scheduler with a starvation guard. A
// lane whose oldest pending entry exceeds `cfg.starvationGuardMs` is promoted
// for one pick regardless of priority, preventing low-priority lanes from
// being starved indefinitely under sustained high-priority load.
//
// Combined with `RequestTag::interruptable = true`, lower-priority queued
// requests are preemptively cancelled when a higher-priority request lands,
// satisfying the "emergency-stop preempts polling within 50 ms" requirement.
class CORE_EXPORT PriorityScheduler : public RequestScheduler {
public:
    explicit PriorityScheduler(SchedulerConfig cfg);
    ~PriorityScheduler() override;

    CORE_DISABLE_COPY_MOVE(PriorityScheduler)

    SubmitResult   submit(RequestTag tag, std::function<void()> work) override;
    SubmitResult   submitAsync(RequestTag tag, AsyncWork work) override;
    void           setDelayFn(DelayFn fn) override;
    void           stopAsync() override;
    int            cancelModule(std::string const& moduleId) override;
    SchedulerStats stats() const override;

private:
    std::unique_ptr<RequestScheduler> m_impl;
};

} // namespace core::sched
