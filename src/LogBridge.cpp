// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/qml/LogBridge.h"

#include "core/log/Logger.h"

namespace core::qml {

LogBridge::LogBridge(log::Logger& logger, QObject* parent)
    : QObject(parent), m_logger(logger) {}

LogBridge::~LogBridge() = default;

void LogBridge::trace(QString category, QString message) {
    m_logger.logf(log::LogLevel::Trace, std::move(category),
                  QStringLiteral("qml"), std::move(message));
}
void LogBridge::debug(QString category, QString message) {
    m_logger.logf(log::LogLevel::Debug, std::move(category),
                  QStringLiteral("qml"), std::move(message));
}
void LogBridge::info(QString category, QString message) {
    m_logger.logf(log::LogLevel::Info, std::move(category),
                  QStringLiteral("qml"), std::move(message));
}
void LogBridge::warn(QString category, QString message) {
    m_logger.logf(log::LogLevel::Warn, std::move(category),
                  QStringLiteral("qml"), std::move(message));
}
void LogBridge::error(QString category, QString message) {
    m_logger.logf(log::LogLevel::Error, std::move(category),
                  QStringLiteral("qml"), std::move(message));
}

void LogBridge::operation(QString action, QString target,
                          QVariant oldValue, QVariant newValue,
                          QString result, QString note) {
    log::OperationRecord rec;
    rec.actor    = QStringLiteral("ui:user");
    rec.action   = std::move(action);
    rec.target   = std::move(target);
    rec.oldValue = std::move(oldValue);
    rec.newValue = std::move(newValue);
    rec.result   = std::move(result);
    rec.note     = std::move(note);
    m_logger.logOperation(std::move(rec));
}

} // namespace core::qml
