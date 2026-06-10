// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QList>
#include <QString>
#include <QtSerialBus/QModbusDataUnit>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"
#include "core/sched/RequestScheduler.h"

namespace core::transport { class Transport; }

namespace core::module {

// Command — one-shot trigger that writes a predefined sequence of values
// through the scheduler. Used for UI-driven actions (emergency stop, manual
// reset) where the operator's intent translates to a fixed write payload.
//
// Each write is submitted independently so the scheduler's priority lanes
// can preempt any pending Poll / SinkWindow flush.
class CORE_EXPORT Command : public FunctionalModule {
public:
    struct Entry {
        QModbusDataUnit::RegisterType table = QModbusDataUnit::HoldingRegisters;
        int     address = 0;
        quint16 value   = 0;
    };
    struct Config {
        QString          moduleId;
        sched::Priority  priority      = sched::Priority::High;
        bool             interruptable = false;
        QList<Entry>     writes;
    };

    Command(Config cfg, transport::Transport& transport);
    ~Command() override;

    CORE_DISABLE_COPY_MOVE(Command)

    // Issue the configured writes through the transport's scheduler (blocking,
    // synchronous). Returns the *first failure* (or Ok if all writes
    // succeeded). Retained for tests and synchronous callers.
    sched::SubmitResult execute();

    // Event-driven, non-blocking equivalent: each write is handed to the
    // scheduler's async path (it serialises them in order) and the call
    // returns immediately. This is the production entry point on a transport
    // whose scheduler is in async mode (i.e. also driven by PollRange etc.) —
    // a synchronous execute() would be rejected there.
    void executeAsync();

    int writeCount() const noexcept;

    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override { return 0; }
    void driveTick()         override {}

private:
    transport::Transport* m_transport;
    Config                m_cfg;
};

} // namespace core::module
