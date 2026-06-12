// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include "core/base/RegisterTable.h"
#include "core/core_global.h"
#include "core/dp/State.h"     // dp::Timestamp
#include "core/dp/Value.h"     // dp::Value
#include "core/sched/SchedulerTypes.h"

namespace core::bus {

// ---------------------------------------------------------------------------
// Standard events published on EventBus by Core subsystems.
// Plugins / UI subscribe to these to react to system state changes.
//
// These are the Qt-free event payloads: std::string / dp::Value / dp::Timestamp
// carry no QtCore types. The Qt layer (attribute adapters, QML bridges) marshals
// them back to QString / QVariant / QDateTime via dp::ValueQt.h / dp::TimeQt.h.
// ---------------------------------------------------------------------------

struct DpChanged {
    std::string    id;
    dp::Value      value;
    dp::Timestamp  timestamp;
};

enum class TransportEventKind {
    Connected,
    Disconnected,
    CircuitOpened,
    CircuitClosed,
    CircuitHalfOpen,
    ReadCompleted,
    WriteCompleted,
};

struct TransportEvent {
    std::string         transportId;
    TransportEventKind  kind;
    std::string         payload;   // optional diagnostic text (e.g. error); empty if none
};

struct CoreReady {};
struct CoreStopping {};

// Published by ModbusTcpServerTransport when an operator-box client writes
// to one of its watched register ranges. Subscribers (router / sink window
// staging) use it to mirror operator intent into PLC-bound Transports.
struct ServerWriteEvent {
    std::string                        transportId;   // the server's id
    core::RegisterTable                table = core::RegisterTable::HoldingRegister;
    int                                startAddress;
    core::RegisterWords                     values;
};

// Periodic snapshot of a Transport's scheduler — published by Core's stats
// pump on `scheduler_stats_publish_interval_ms` cadence (default 1000 ms).
// QML / plugin dashboards subscribe to this to surface queue depth, inflight
// count, p50/p99 latency and circuit-breaker state.
struct SchedulerStatsEvent {
    std::string           transportId;
    sched::SchedulerStats stats;
};

} // namespace core::bus
