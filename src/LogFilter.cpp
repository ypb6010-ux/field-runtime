#include "core/log/LogFilter.h"

namespace core::log {

void LogFilter::setDefault(bool enabled, LogLevel minLevel) {
    m_default = Rule{enabled, minLevel};
}

void LogFilter::setDefaultMinLevel(LogLevel minLevel) {
    m_default.minLevel = minLevel;
}

void LogFilter::setCategory(QString const& category, bool enabled, LogLevel minLevel) {
    m_categories.insert(category, Rule{enabled, minLevel});
}

void LogFilter::clearCategory(QString const& category) {
    m_categories.remove(category);
}

bool LogFilter::hasCategory(QString const& category) const {
    return m_categories.contains(category);
}

LogFilter::Rule LogFilter::ruleFor(QString const& category) const {
    auto it = m_categories.constFind(category);
    return it != m_categories.constEnd() ? it.value() : m_default;
}

bool LogFilter::passes(QString const& category, LogLevel level) const {
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
    for (auto it = over.m_categories.constBegin(); it != over.m_categories.constEnd(); ++it) {
        m_categories.insert(it.key(), it.value());
    }
}

} // namespace core::log
