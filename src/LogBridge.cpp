// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/qml/LogBridge.h"

#include "core/dp/ValueQt.h"
#include "core/log/Logger.h"

namespace core::qml {

LogBridge::LogBridge(log::Logger& logger, QObject* parent)
    : QObject(parent), m_logger(logger) {}

LogBridge::~LogBridge() = default;

void LogBridge::trace(QString category, QString message) {
    m_logger.logf(log::LogLevel::Trace, category.toStdString(),
                  "qml", message.toStdString());
}
void LogBridge::debug(QString category, QString message) {
    m_logger.logf(log::LogLevel::Debug, category.toStdString(),
                  "qml", message.toStdString());
}
void LogBridge::info(QString category, QString message) {
    m_logger.logf(log::LogLevel::Info, category.toStdString(),
                  "qml", message.toStdString());
}
void LogBridge::warn(QString category, QString message) {
    m_logger.logf(log::LogLevel::Warn, category.toStdString(),
                  "qml", message.toStdString());
}
void LogBridge::error(QString category, QString message) {
    m_logger.logf(log::LogLevel::Error, category.toStdString(),
                  "qml", message.toStdString());
}

void LogBridge::operation(QString action, QString target,
                          QVariant oldValue, QVariant newValue,
                          QString result, QString note) {
    log::OperationRecord rec;
    rec.actor    = "ui:user";
    rec.action   = action.toStdString();
    rec.target   = target.toStdString();
    rec.oldValue = dp::fromQVariant(oldValue);
    rec.newValue = dp::fromQVariant(newValue);
    rec.result   = result.toStdString();
    rec.note     = note.toStdString();
    m_logger.logOperation(std::move(rec));
}

} // namespace core::qml
