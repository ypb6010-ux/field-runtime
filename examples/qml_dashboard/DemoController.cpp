#include "DemoController.h"

#include <QJsonObject>

#include "core/ICore.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/log/Logger.h"
#include "core/transport/Transport.h"
#include "core/transport/TransportTypes.h"

#ifdef DASHBOARD_HAS_PERSISTENCE
#include "core/persistence/Persistence.h"
#endif

namespace {

QString kindText(core::transport::TransportKind k) {
    using K = core::transport::TransportKind;
    switch (k) {
        case K::ModbusTcpClient: return QStringLiteral("Modbus TCP");
        case K::ModbusTcpServer: return QStringLiteral("Modbus Server");
        case K::ModbusRtu:       return QStringLiteral("Modbus RTU");
        case K::OpcUaClient:     return QStringLiteral("OPC UA");
        case K::MqttClient:      return QStringLiteral("MQTT");
        case K::MqttPahoClient:  return QStringLiteral("MQTT (paho)");
        case K::S7Client:        return QStringLiteral("S7");
    }
    return QStringLiteral("?");
}

QString stateText(core::transport::ConnectionState s) {
    using S = core::transport::ConnectionState;
    switch (s) {
        case S::Disconnected: return QStringLiteral("Disconnected");
        case S::Connecting:   return QStringLiteral("Connecting");
        case S::Connected:    return QStringLiteral("Connected");
        case S::Error:        return QStringLiteral("Error");
    }
    return QStringLiteral("?");
}

} // namespace

DemoController::DemoController(core::ICore& core, core::persist::Persistence* db,
                               QObject* parent)
    : QObject(parent), m_core(core), m_db(db) {
    m_timer.setInterval(300);
    QObject::connect(&m_timer, &QTimer::timeout, this, &DemoController::refresh);
    m_timer.start();
    refresh();
}

void DemoController::refresh() {
    // Transports — kind + connection state + scheduler pressure.
    QVariantList transports;
    for (auto const& id : m_core.transportIds()) {
        auto* t = m_core.transport(id);
        if (!t) continue;
        auto st = t->scheduler().stats();
        transports.append(QVariantMap{
            {QStringLiteral("id"),       id},
            {QStringLiteral("kind"),     kindText(t->kind())},
            {QStringLiteral("state"),    stateText(t->state())},
            {QStringLiteral("connected"),
                 t->state() == core::transport::ConnectionState::Connected},
            {QStringLiteral("queue"),    st.queueDepth},
            {QStringLiteral("p99"),      st.p99LatencyMs},
        });
    }
    m_transports = std::move(transports);

    // Datapoints — value, state, and the source transport's kind.
    QVariantList datapoints;
    for (auto const& dp : m_core.datapoints().all()) {
        QString srcId, srcKind;
        if (dp->source().has_value()) {
            srcId = dp->source()->transport;
            if (auto* t = m_core.transport(srcId)) srcKind = kindText(t->kind());
        }
        datapoints.append(QVariantMap{
            {QStringLiteral("id"),         dp->id()},
            {QStringLiteral("value"),      dp->value().toString()},
            {QStringLiteral("state"),      dp->stateText()},
            {QStringLiteral("sourceId"),   srcId},
            {QStringLiteral("sourceKind"), srcKind.isEmpty()
                                              ? QStringLiteral("—") : srcKind},
        });
    }
    m_datapoints = std::move(datapoints);

    m_dropped = int(m_core.logger().droppedCount());
    emit tick();
}

void DemoController::setLogLevel(int level) {
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    m_core.logger().setThreshold(static_cast<core::log::LogLevel>(level));
    m_core.logger().logf(core::log::LogLevel::Info, QStringLiteral("ui"),
        QStringLiteral("dashboard"),
        QStringLiteral("log threshold set to %1")
            .arg(QString::fromLatin1(core::log::levelName(
                static_cast<core::log::LogLevel>(level)))));
}

void DemoController::emitOperation(QString action, QString target) {
    core::log::OperationRecord op;
    op.actor  = QStringLiteral("ui:user");
    op.action = std::move(action);
    op.target = std::move(target);
    op.result = QStringLiteral("ok");
    m_core.logger().logOperation(std::move(op));
}

void DemoController::writeDatapoint(QString id, QVariant value) {
    if (auto dp = m_core.datapoints().find(id)) dp->write(std::move(value));
}

void DemoController::appendLog(QVariantMap record) {
    m_logs.prepend(record);
    while (m_logs.size() > 300) m_logs.removeLast();
    emit logsChanged();
}

#ifdef DASHBOARD_HAS_PERSISTENCE
QVariantMap DemoController::queryTelemetry(QString tag, QString start,
                                           QString end, int page) {
    return m_db ? m_db->getTelemetry(tag, start, end, page).toVariantMap()
                : QVariantMap{};
}
QVariantMap DemoController::queryOperation(QString start, QString end, int page) {
    return m_db ? m_db->getOperationLog(start, end, page).toVariantMap()
                : QVariantMap{};
}
QVariantMap DemoController::querySystem(int minLevel, QString start,
                                        QString end, int page) {
    return m_db ? m_db->getSystemLog(minLevel, start, end, page).toVariantMap()
                : QVariantMap{};
}
#else
QVariantMap DemoController::queryTelemetry(QString, QString, QString, int) { return {}; }
QVariantMap DemoController::queryOperation(QString, QString, int) { return {}; }
QVariantMap DemoController::querySystem(int, QString, QString, int) { return {}; }
#endif
