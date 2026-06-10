#pragma once

#include <QDateTime>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "core/core_global.h"

namespace core::log {

// Severity for system (diagnostic) logs. Operation logs are not filtered by
// level — they are an audit trail and always recorded.
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
};

CORE_EXPORT const char* levelName(LogLevel level) noexcept;
CORE_EXPORT LogLevel    levelFromString(QString const& s) noexcept;

// System / diagnostic record — free-text message plus optional structured kv.
// Produced by transports, scheduler, modules, config, core lifecycle.
struct LogRecord {
    QDateTime   ts;
    LogLevel    level = LogLevel::Info;
    QString     category;   // "transport" / "scheduler" / "module" / ...
    QString     source;     // transport id / module id / "qml"
    QString     message;
    QVariantMap fields;     // optional structured context
};

// Run / operation / audit record — structured business event. Produced by the
// router (operator-box writes), commands, and the UI (actor = "ui:user").
// Gated by the category axis of LogFilter only (no severity); the audit trail
// is preserved by routing it to a pass-all sink, not by bypassing the filter.
struct OperationRecord {
    QDateTime ts;
    QString   actor;        // "ui:user" / "operator-box" / "auto"
    QString   action;       // "write" / "command" / "reset" / "connect" ...
    QString   target;       // datapoint id / transport id
    QVariant  oldValue;
    QVariant  newValue;
    QString   result;       // "ok" / "failed" / "rejected"
    QString   note;
    QString   category = QStringLiteral("audit");  // LogFilter category axis
    QString   eventKey;     // dedup key, e.g. "server-write:0"
};

} // namespace core::log
