// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <string>

#include "core/core_global.h"
#include "core/sched/SchedulerTypes.h"

namespace core::module {

// Base class for any long-lived runtime unit that issues requests via a
// Transport's scheduler: PollRange, SinkWindow, Heartbeat, AckWatch, Command.
//
// Lifecycle: created via configuration, registered with ModuleRegistry,
// start() called by Core, stop() called at shutdown (RAII).
class CORE_EXPORT FunctionalModule {
public:
    virtual ~FunctionalModule() = default;

    std::string const&    id()          const noexcept { return m_id; }
    std::string const&    transportId() const noexcept { return m_transportId; }
    sched::Priority       priority()    const noexcept { return m_priority; }
    sched::ModuleStats    stats()       const          { return m_stats; }

    virtual void start()  = 0;
    virtual void stop()   = 0;
    virtual void pause()  = 0;
    virtual void resume() = 0;

    // Tick interface — ModuleRegistry's timer driver calls `driveTick()` at
    // the module's preferred cadence. `tickPeriodMs() == 0` disables auto
    // driving (Command and AckWatch are invoked imperatively).
    virtual int  tickPeriodMs() const = 0;
    virtual void driveTick()          = 0;

protected:
    std::string         m_id;
    std::string         m_transportId;
    sched::Priority     m_priority = sched::Priority::Normal;
    sched::ModuleStats  m_stats;
};

} // namespace core::module
