// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <QList>
#include <QString>
#include <QtSerialBus/QModbusDataUnit>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/TransportTypes.h"

namespace core::transport { class Transport; }

namespace core::module {

// Heartbeat — periodically writes a fixed payload to a specific Modbus
// address to maintain master-alive on PLCs that watchdog the connection.
//
// Unlike SinkWindow, the payload never depends on staged datapoint state —
// it's the same constant on every tick. Use a SinkWindow with
// `keepAlivePeriodMs` if you want to keep mutable control state alive.
class CORE_EXPORT Heartbeat : public FunctionalModule {
public:
    struct Config {
        QString                       moduleId;
        QModbusDataUnit::RegisterType table     = QModbusDataUnit::HoldingRegisters;
        int                           address   = 0;
        QList<quint16>                values;
        int                           periodMs  = 0;
        sched::Priority               priority  = sched::Priority::Low;
    };

    Heartbeat(Config cfg, transport::Transport& transport);
    ~Heartbeat() override;

    CORE_DISABLE_COPY_MOVE(Heartbeat)

    // Drive one heartbeat decision. Returns the scheduler submission result
    // when a write actually fires, or Ok with zero latency if the period
    // hasn't elapsed yet.
    sched::SubmitResult onTick();

    int periodMs() const noexcept;

    void start()  override;
    void stop()   override;
    void pause()  override;
    void resume() override;
    int  tickPeriodMs() const override { return m_cfg.periodMs; }
    // Event-driven, non-blocking write via the scheduler's async path.
    void driveTick()         override;

private:
    transport::Transport*                  m_transport;
    Config                                  m_cfg;
    std::atomic<bool>                       m_started{false};
    std::chrono::steady_clock::time_point   m_lastSentAt;
    std::atomic<bool>                       m_inFlight{false};
};

} // namespace core::module
