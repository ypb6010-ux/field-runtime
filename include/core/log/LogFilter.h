// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <unordered_map>

#include "core/core_global.h"
#include "core/log/LogTypes.h"

namespace core::log {

// Dual-axis log gate.
//
// A record passes when its category is ENABLED *and* its level meets that
// category's minimum. Categories without an explicit rule fall back to the
// default rule. The two axes are independent:
//   - category axis (enable/disable)  → "show this kind at all?"
//   - level axis    (minLevel)        → "and only from this severity up"
//
// OperationRecord has no meaningful severity, so it is gated by the category
// axis ONLY: an audit record passes unless its category is explicitly
// disabled. This keeps the audit trail flowing through a permissive baseline
// (and into a pass-all file sink) even when the level threshold is raised,
// while still letting a business filter hide it by disabling its category.
//
// LogFilter is a plain value type (copyable). Callers that share one across
// threads (Logger, sinks) guard it themselves.
class CORE_EXPORT LogFilter {
public:
    struct Rule {
        bool     enabled  = true;
        LogLevel minLevel = LogLevel::Info;
    };

    LogFilter() = default;
    explicit LogFilter(Rule defaultRule)
        : m_default(defaultRule) {
    }

    // ── configuration ──────────────────────────────────────────────────
    void setDefault(bool enabled, LogLevel minLevel);
    void setDefaultMinLevel(LogLevel minLevel);   // keep enabled, raise/lower floor
    void setCategory(std::string const& category, bool enabled, LogLevel minLevel);
    void clearCategory(std::string const& category);  // fall back to default

    Rule defaultRule() const { return m_default; }
    bool hasCategory(std::string const& category) const;
    Rule ruleFor(std::string const& category) const;

    // ── evaluation ─────────────────────────────────────────────────────
    bool passes(std::string const& category, LogLevel level) const;
    bool passes(LogRecord const& r) const;
    bool passes(OperationRecord const& r) const;   // category axis only

    // ── inheritance / override ─────────────────────────────────────────
    // inherit = copy the base as a starting point; overrideWith merges
    // another filter on top (its default + each of its categories win).
    static LogFilter inherit(LogFilter const& base) { return base; }
    void overrideWith(LogFilter const& over);

private:
    Rule                                  m_default{};
    std::unordered_map<std::string, Rule> m_categories;
};

} // namespace core::log
