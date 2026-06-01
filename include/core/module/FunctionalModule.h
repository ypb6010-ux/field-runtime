#pragma once

#include <atomic>
#include <QString>

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

    QString               id()          const noexcept { return m_id; }
    QString               transportId() const noexcept { return m_transportId; }
    sched::Priority       priority()    const noexcept { return m_priority; }
    sched::ModuleStats    stats()       const          { return m_stats; }

    virtual void start()  = 0;
    virtual void stop()   = 0;
    virtual void pause()  = 0;
    virtual void resume() = 0;

protected:
    QString             m_id;
    QString             m_transportId;
    sched::Priority     m_priority = sched::Priority::Normal;
    sched::ModuleStats  m_stats;
};

} // namespace core::module
