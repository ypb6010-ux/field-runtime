// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include "core/base/RegisterTable.h"
#include "core/core_global.h"
#include "core/dp/State.h"     // dp::Timestamp
#include "core/dp/Value.h"     // dp::Value
#include "core/sched/SchedulerTypes.h"
#include "core/transport/TransportTypes.h"

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

// A successful PollRange publishes the untouched register image only after
// datapoint application is complete. Bridge consumers can mirror this image
// without re-encoding scaled/bitfield presentation values.
struct PollRangeCompleted {
    std::string          moduleId;
    std::string          transportId;
    core::RegisterTable  table = core::RegisterTable::HoldingRegister;
    int                  startAddress = 0;
    int                  count = 0;
    core::RegisterWords  values;
    dp::Timestamp        completedAt;
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
    PeerSessionChangeKind   kind = PeerSessionChangeKind::Connected;
    transport::PeerSession  session;
    std::string             reason;
    dp::Timestamp           changedAt;
};

struct SchedulerCircuitChanged {
    std::string          transportId;
    sched::CircuitState  before = sched::CircuitState::Closed;
    sched::CircuitState  after = sched::CircuitState::Closed;
    dp::Timestamp        changedAt;
};

struct ConfigReloadStarted {
    std::string path;
};

struct ConfigReloadSucceeded {
    std::string path;
};

struct ConfigReloadFailed {
    std::string path;
    std::string reason;
};

struct DatapointModelRebuilt {
    std::uint64_t generation = 0;
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
    std::string                        sessionId;
    std::string                        sourceAddress;
    int                                unitId = 0;
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
