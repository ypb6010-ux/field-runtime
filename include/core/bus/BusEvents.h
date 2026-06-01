#pragma once

#include <QDateTime>
#include <QString>
#include <QVariant>

#include "core/core_global.h"

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
    QString             transportId;
    TransportEventKind  kind;
    QVariant            payload;
};

struct CoreReady {};
struct CoreStopping {};

} // namespace core::bus
