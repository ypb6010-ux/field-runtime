// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/core_global.h"
#include "core/sched/RequestScheduler.h"

namespace core::sched {

// CreditScheduler — full-duplex aware scheduler that holds up to
// `cfg.maxInflight` in-flight requests concurrently. Used with transports
// that can issue multiple requests in parallel without violating physical
// constraints (OPC UA over TCP, MQTT, gRPC, raw TCP). Priority lanes and
// round-robin-within-lane semantics are identical to SerialScheduler; the
// only difference is the inflight ceiling.
class CORE_EXPORT CreditScheduler : public RequestScheduler {
public:
    explicit CreditScheduler(SchedulerConfig cfg);
    ~CreditScheduler() override;

    CORE_DISABLE_COPY_MOVE(CreditScheduler)

    SubmitResult   submit(RequestTag tag, std::function<void()> work) override;
    SubmitResult   submitAsync(RequestTag tag, AsyncWork work) override;
    void           setDelayFn(DelayFn fn) override;
    void           stopAsync() override;
    int            cancelModule(QString const& moduleId) override;
    SchedulerStats stats() const override;

private:
    std::unique_ptr<RequestScheduler> m_impl;
};

} // namespace core::sched
