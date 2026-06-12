// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include "core/dp/TimeQt.h"
#include "core/dp/ValueQt.h"
#include "core/log/ILogSink.h"
#include "core/log/Logger.h"

#include "GatewayController.h"

// Feeds the live log view in QML. write() runs on the logger's dispatch thread,
// so each record is marshalled to the GUI thread via a queued call. Adapts the
// Qt-free LogRecord / OperationRecord (std::string + std::chrono) to QVariantMap.
class UiLogSink : public core::log::ILogSink {
public:
    explicit UiLogSink(GatewayController* c) : m_ctrl(c) {}

    void write(core::log::LogRecord const& r) override {
        post(QVariantMap{
            {QStringLiteral("kind"),     QStringLiteral("system")},
            {QStringLiteral("ts"),       hms(r.ts)},
            {QStringLiteral("badge"),    QString::fromLatin1(core::log::levelName(r.level))},
            {QStringLiteral("category"), QString::fromStdString(r.category)},
            {QStringLiteral("source"),   QString::fromStdString(r.source)},
            {QStringLiteral("text"),     QString::fromStdString(r.message)},
        });
    }

    void write(core::log::OperationRecord const& r) override {
        post(QVariantMap{
            {QStringLiteral("kind"),     QStringLiteral("operation")},
            {QStringLiteral("ts"),       hms(r.ts)},
            {QStringLiteral("badge"),    QStringLiteral("OP")},
            {QStringLiteral("category"), QString::fromStdString(r.actor)},
            {QStringLiteral("source"),   QString::fromStdString(r.target)},
            {QStringLiteral("text"),
                 QStringLiteral("%1 → %2 [%3]").arg(QString::fromStdString(r.action),
                                                    QString::fromStdString(r.target),
                                                    QString::fromStdString(r.result))},
        });
    }

private:
    static QString hms(core::log::LogTime ts) {
        return core::dp::toQDateTime(ts).toString(QStringLiteral("HH:mm:ss.zzz"));
    }

    void post(QVariantMap m) {
        if (!m_ctrl) return;
        QMetaObject::invokeMethod(m_ctrl, "appendLog", Qt::QueuedConnection,
                                  Q_ARG(QVariantMap, m));
    }

    QPointer<GatewayController> m_ctrl;
};
