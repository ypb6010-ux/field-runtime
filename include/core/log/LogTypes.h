// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <map>
#include <string>

#include "core/core_global.h"
#include "core/dp/Value.h"

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
CORE_EXPORT LogLevel    levelFromString(std::string const& s) noexcept;

// Qt-free wall-clock timestamp for log records. The Qt sinks / QML bridge
// marshal it to QDateTime via dp::TimeQt.h.
using LogTime = std::chrono::system_clock::time_point;

// System / diagnostic record — free-text message plus optional structured kv.
// Produced by transports, scheduler, modules, config, core lifecycle.
struct LogRecord {
    LogTime     ts{};
    LogLevel    level = LogLevel::Info;
    std::string category;   // "transport" / "scheduler" / "module" / ...
    std::string source;     // transport id / module id / "qml"
    std::string message;
    std::map<std::string, dp::Value> fields;   // optional structured context
};

// Run / operation / audit record — structured business event. Produced by the
// router (operator-box writes), commands, and the UI (actor = "ui:user").
// Gated by the category axis of LogFilter only (no severity); the audit trail
// is preserved by routing it to a pass-all sink, not by bypassing the filter.
struct OperationRecord {
    LogTime     ts{};
    std::string actor;      // "ui:user" / "operator-box" / "auto"
    std::string action;     // "write" / "command" / "reset" / "connect" ...
    std::string target;     // datapoint id / transport id
    dp::Value   oldValue;
    dp::Value   newValue;
    std::string result;     // "ok" / "failed" / "rejected"
    std::string note;
    std::string category = "audit";  // LogFilter category axis
    std::string eventKey;   // dedup key, e.g. "server-write:0"
};

} // namespace core::log
