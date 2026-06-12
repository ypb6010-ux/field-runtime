// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/LogFilter.h"

namespace core::log {

void LogFilter::setDefault(bool enabled, LogLevel minLevel) {
    m_default = Rule{enabled, minLevel};
}

void LogFilter::setDefaultMinLevel(LogLevel minLevel) {
    m_default.minLevel = minLevel;
}

void LogFilter::setCategory(std::string const& category, bool enabled, LogLevel minLevel) {
    m_categories.insert_or_assign(category, Rule{enabled, minLevel});
}

void LogFilter::clearCategory(std::string const& category) {
    m_categories.erase(category);
}

bool LogFilter::hasCategory(std::string const& category) const {
    return m_categories.contains(category);
}

LogFilter::Rule LogFilter::ruleFor(std::string const& category) const {
    auto it = m_categories.find(category);
    return it != m_categories.end() ? it->second : m_default;
}

bool LogFilter::passes(std::string const& category, LogLevel level) const {
    Rule const r = ruleFor(category);
    return r.enabled && level >= r.minLevel;
}

bool LogFilter::passes(LogRecord const& r) const {
    return passes(r.category, r.level);
}

bool LogFilter::passes(OperationRecord const& r) const {
    // Audit records have no severity; gate on the category axis only.
    return ruleFor(r.category).enabled;
}

void LogFilter::overrideWith(LogFilter const& over) {
    m_default = over.m_default;
    for (auto const& [category, rule] : over.m_categories) {
        m_categories.insert_or_assign(category, rule);
    }
}

} // namespace core::log
