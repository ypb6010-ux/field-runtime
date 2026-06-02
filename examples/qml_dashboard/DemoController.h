#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace core { class ICore; }
namespace core::persist { class Persistence; }

// View-model bridging the new Core (transports / datapoints / logger) and the
// optional Persistence module to QML. Everything QML needs is exposed here as
// properties (polled) or Q_INVOKABLE calls; the live log stream is fed in from
// a UiLogSink via the appendLog slot (queued, thread-safe).
class DemoController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList transports READ transports NOTIFY tick)
    Q_PROPERTY(QVariantList datapoints READ datapoints NOTIFY tick)
    Q_PROPERTY(QVariantList logs       READ logs       NOTIFY logsChanged)
    Q_PROPERTY(int          dropped    READ dropped    NOTIFY tick)
    Q_PROPERTY(bool         dbAvailable READ dbAvailable CONSTANT)
public:
    DemoController(core::ICore& core, core::persist::Persistence* db,
                   QObject* parent = nullptr);

    QVariantList transports() const { return m_transports; }
    QVariantList datapoints() const { return m_datapoints; }
    QVariantList logs()       const { return m_logs; }
    int          dropped()    const { return m_dropped; }
    bool         dbAvailable() const { return m_db != nullptr; }

    // ── UI actions ─────────────────────────────────────────────────────
    Q_INVOKABLE void setLogLevel(int level);                  // 0..5
    Q_INVOKABLE void emitOperation(QString action, QString target);
    Q_INVOKABLE void writeDatapoint(QString id, QVariant value);

    // ── history queries (return {pages,page,limit,data:[...]}) ─────────
    Q_INVOKABLE QVariantMap queryTelemetry(QString tag, QString start,
                                           QString end, int page);
    Q_INVOKABLE QVariantMap queryOperation(QString start, QString end, int page);
    Q_INVOKABLE QVariantMap querySystem(int minLevel, QString start,
                                        QString end, int page);

public slots:
    // Invoked (queued) from UiLogSink on the logger's dispatch thread.
    void appendLog(QVariantMap record);

signals:
    void tick();
    void logsChanged();

private:
    void refresh();

    core::ICore&             m_core;
    core::persist::Persistence* m_db = nullptr;
    QTimer                   m_timer;
    QVariantList             m_transports;
    QVariantList             m_datapoints;
    QVariantList             m_logs;       // newest first, capped
    int                      m_dropped = 0;
};
