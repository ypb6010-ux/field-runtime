// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariant>
#include <QtSerialBus/QModbusDataUnit>

#include "core/core_global.h"
#include "core/sched/SchedulerTypes.h"
#include "core/transport/TransportTypes.h"

namespace core::bus {

// ---------------------------------------------------------------------------
// Standard events published on EventBus by Core subsystems.
// Plugins / UI subscribe to these to react to system state changes.
// ---------------------------------------------------------------------------

struct DpChanged {
    QString    id;
    QVariant   value;
    QDateTime  timestamp;
};

// Quality/state transitions (including recovery to Ok) are separate from
// DpChanged so value consumers (commands, plugin inputs, acknowledgements) are
// not retriggered by a quality transition that retains the previous value.
struct DpStateChanged {
    QString    id;
    QVariant   value;
    int        state = 0; // dp::DpState numeric value
    QDateTime  timestamp;
};

struct TransportStateChanged {
    transport::TransportStatus before;
    transport::TransportStatus after;
};

enum class PeerSessionChangeKind {
    Connected,
    Disconnected,
};

struct PeerSessionChanged {
    PeerSessionChangeKind      kind = PeerSessionChangeKind::Connected;
    transport::PeerSession     session;
    QString                    reason;
    QDateTime                  changedAt;
};

struct SchedulerCircuitChanged {
    QString               transportId;
    sched::CircuitState   before = sched::CircuitState::Closed;
    sched::CircuitState   after  = sched::CircuitState::Closed;
    QDateTime             changedAt;
};

// A successful PollRange publishes the untouched register image only after
// datapoint application is complete. Bridge consumers can mirror this image
// without re-encoding scaled/bitfield presentation values.
struct PollRangeCompleted {
    QString                            moduleId;
    QString                            transportId;
    QModbusDataUnit::RegisterType      table;
    int                                startAddress = 0;
    int                                count = 0;
    QList<quint16>                     values;
    QDateTime                          completedAt;
};

struct ConfigReloadStarted {
    QString path;
};

struct ConfigReloadSucceeded {
    QString path;
};

struct ConfigReloadFailed {
    QString path;
    QString reason;
};

// Consumers holding raw/QML datapoint references must reacquire them after a
// successful reload. The generation is monotonically increasing per Core.
struct DatapointModelRebuilt {
    quint64 generation = 0;
};

struct CoreReady {};
struct CoreStopping {};

// Published by ModbusTcpServerTransport when an operator-box client writes
// to one of its watched register ranges. Subscribers (router / sink window
// staging) use it to mirror operator intent into PLC-bound Transports.
struct ServerWriteEvent {
    QString                            transportId;   // the server's id
    QModbusDataUnit::RegisterType      table;
    int                                startAddress;
    QList<quint16>                     values;
};

// Periodic snapshot of a Transport's scheduler — published by Core's stats
// pump on `scheduler_stats_publish_interval_ms` cadence (default 1000 ms).
// QML / plugin dashboards subscribe to this to surface queue depth, inflight
// count, p50/p99 latency and circuit-breaker state.
struct SchedulerStatsEvent {
    QString               transportId;
    sched::SchedulerStats stats;
};

} // namespace core::bus
