#pragma once

#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>

#include "core/log/ILogSink.h"
#include "core/log/Logger.h"

#include "DemoController.h"

// A custom ILogSink that feeds the live log view in QML — demonstrates the
// registerable-sink extension point. write() runs on the logger's dispatch
// thread, so each record is marshalled to the GUI thread via a queued call.
class UiLogSink : public core::log::ILogSink {
public:
    explicit UiLogSink(DemoController* c) : m_ctrl(c) {}

    void write(core::log::LogRecord const& r) override {
        post(QVariantMap{
            {QStringLiteral("kind"),     QStringLiteral("system")},
            {QStringLiteral("ts"),       r.ts.toString(QStringLiteral("HH:mm:ss.zzz"))},
            {QStringLiteral("level"),    int(r.level)},
            {QStringLiteral("badge"),    QString::fromLatin1(core::log::levelName(r.level))},
            {QStringLiteral("category"), r.category},
            {QStringLiteral("source"),   r.source},
            {QStringLiteral("text"),     r.message},
        });
    }

    void write(core::log::OperationRecord const& r) override {
        post(QVariantMap{
            {QStringLiteral("kind"),     QStringLiteral("operation")},
            {QStringLiteral("ts"),       r.ts.toString(QStringLiteral("HH:mm:ss.zzz"))},
            {QStringLiteral("level"),    -1},
            {QStringLiteral("badge"),    QStringLiteral("OP")},
            {QStringLiteral("category"), r.actor},
            {QStringLiteral("source"),   r.target},
            {QStringLiteral("text"),
                 QStringLiteral("%1 → %2 [%3]").arg(r.action, r.target, r.result)},
        });
    }

private:
    void post(QVariantMap m) {
        if (!m_ctrl) return;
        QMetaObject::invokeMethod(m_ctrl, "appendLog", Qt::QueuedConnection,
                                  Q_ARG(QVariantMap, m));
    }

    QPointer<DemoController> m_ctrl;
};
