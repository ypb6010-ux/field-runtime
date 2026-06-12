// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/core_global.h"
#include "core/sched/RequestScheduler.h"

namespace core::sched {

// Half-duplex aware scheduler. Holds at most one in-flight request, enforces
// an optional inter-request gap, and serves pending requests by descending
// priority (Critical → High → Normal → Low). Within a single priority lane,
// requests are dispatched round-robin by `moduleId` so a high-frequency
// module cannot starve other modules at the same priority.
//
// This is the default scheduler for Modbus TCP transports backed by a 485
// gateway (the most common deployment in the codebase) where physical
// concurrency would otherwise return `ServerDeviceBusy` errors.
class CORE_EXPORT SerialScheduler : public RequestScheduler {
public:
    explicit SerialScheduler(SchedulerConfig cfg);
    ~SerialScheduler() override;

    CORE_DISABLE_COPY_MOVE(SerialScheduler)

    SubmitResult   submit(RequestTag tag, std::function<void()> work) override;
    SubmitResult   submitAsync(RequestTag tag, AsyncWork work) override;
    void           setDelayFn(DelayFn fn) override;
    int            cancelModule(std::string const& moduleId) override;
    SchedulerStats stats() const override;

    void stopAsync() override;

    // Test hook — record a synthetic failure to drive the circuit-breaker.
    void recordFailureForTesting();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::sched
