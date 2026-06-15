// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "GatewayController.h"

#include <sstream>

#include <QDir>
#include <QFile>
#include <QModbusDataUnit>
#include <QModbusReply>
#include <QModbusTcpClient>
#include <QUrl>

#include "core/ICore.h"
#include "core/bus/EventBus.h"
#include "core/log/ILogSink.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/Value.h"
#include "core/dp/ValueQt.h"
#include "core/log/Logger.h"
#include "core/transport/Transport.h"

namespace {
constexpr int kSetpointAddr = 20;   // PLC HR where setpoint / operator write lands
constexpr int kPollCount    = 32;   // poll the whole simulated PLC
constexpr int kMaxLogs      = 200;
}  // namespace

GatewayController::GatewayController(quint16 plcPort, quint16 opboxPort, QObject* parent)
    : QObject(parent), m_plcPort(plcPort), m_opboxPort(opboxPort) {
    // Seed a few datapoints matching the SimulatedPlc's driven registers.
    m_config = {
        {QStringLiteral("temperature"), 0, QStringLiteral("S16"), 0.1},
        {QStringLiteral("belt_speed"),  1, QStringLiteral("U16"), 1.0},
        {QStringLiteral("pressure"),    2, QStringLiteral("U16"), 0.01},
        {QStringLiteral("current"),     4, QStringLiteral("U16"), 0.1},
    };
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &GatewayController::refresh);
}

GatewayController::~GatewayController() { stop(); }

void GatewayController::setLogSink(std::shared_ptr<core::log::ILogSink> sink) {
    m_logSink = std::move(sink);
}

void GatewayController::setHost(QString v)   { if (v != m_host)     { m_host = std::move(v); emit configChanged(); } }
void GatewayController::setPlcPort(int v)    { if (v != m_plcPort)  { m_plcPort = v;   emit configChanged(); } }
void GatewayController::setOpboxPort(int v)  { if (v != m_opboxPort){ m_opboxPort = v; emit configChanged(); } }
void GatewayController::setPeriodMs(int v)   { if (v != m_periodMs) { m_periodMs = v;  emit configChanged(); } }

void GatewayController::setForwarding(bool v) {
    if (v == m_forwarding) return;
    m_forwarding = v;
    if (m_core) m_core->setServerForwardEnabled("opbox", v);
    setStatus(v ? QStringLiteral("转发已开启:操作箱写入将下发到 PLC")
                : QStringLiteral("转发已关闭:操作箱写入被拦截,不下发 PLC"));
    emit forwardingChanged();
}

void GatewayController::addPoint(QString id, int address, QString type, double scale) {
    if (id.isEmpty()) return;
    for (auto const& p : m_config) if (p.id == id) return;   // unique id
    m_config.push_back({std::move(id), address, type.isEmpty() ? QStringLiteral("U16") : type,
                        scale == 0.0 ? 1.0 : scale});
    emit configChanged();
}

void GatewayController::removePoint(QString id) {
    for (int i = 0; i < m_config.size(); ++i) {
        if (m_config[i].id == id) { m_config.removeAt(i); emit configChanged(); return; }
    }
}

std::string GatewayController::buildToml() const {
    std::ostringstream o;
    auto qs = [](QString const& s) { return s.toStdString(); };

    o << "[meta]\n"
      << "project = \"modbus_hmi\"\nversion = \"0.1\"\nlog_level = \"info\"\n\n";

    // PLC client.
    o << "[[transport]]\nid = \"plc\"\nkind = \"modbus_tcp_client\"\n"
      << "host = \"" << qs(m_host) << "\"\nport = " << m_plcPort << "\nslave_id = 1\n"
      << "connect_timeout_ms = 500\nreconnect_interval_ms = 2000\n"
      << "[transport.scheduler]\nkind = \"serial\"\ninter_request_gap_ms = 5\n\n";

    // Operator-box Modbus server (the bridge's server side).
    o << "[[transport]]\nid = \"opbox\"\nkind = \"modbus_tcp_server\"\n"
      << "listen_address = \"127.0.0.1\"\nlisten_port = " << m_opboxPort << "\nslave_id = 1\n"
      << "[[transport.listen_ranges]]\ntable = \"HR\"\nrange = [0, " << kPollCount << "]\n\n";

    // Poll the whole register span so configured points + setpoint echo update.
    o << "[[poll_range]]\nmodule_id = \"poll.plc\"\ntransport = \"plc\"\n"
      << "table = \"HR\"\nrange = [0, " << kPollCount << "]\nperiod_ms = " << m_periodMs << "\n\n";

    // Sink window for the downlink setpoint command.
    o << "[[sink_window]]\nmodule_id = \"sw.cmd\"\ntransport = \"plc\"\n"
      << "table = \"HR\"\nrange = [" << kSetpointAddr << ", 2]\npriority = \"High\"\n\n";

    // User-configured status datapoints.
    for (auto const& p : m_config) {
        o << "[[datapoint]]\nid = \"" << qs(p.id) << "\"\nkind = \"Status\"\n"
          << "type = \"" << qs(p.type) << "\"\n"
          << "source = { port = \"plc\", table = \"HR\", addr = " << p.address
          << ", scale = " << p.scale;
        // multi-register types need a word order
        if (p.type == "U32" || p.type == "S32" || p.type == "F32"
         || p.type == "U64" || p.type == "S64" || p.type == "F64") {
            o << ", wordOrder = \"ABCD\"";
        }
        o << " }\n\n";
    }

    // Downlink command datapoint (writes via the sink window to the PLC).
    o << "[[datapoint]]\nid = \"setpoint\"\nkind = \"Command\"\ntype = \"U16\"\n"
      << "sink = { port = \"plc\", table = \"HR\", addr = " << kSetpointAddr
      << ", window = \"sw.cmd\" }\n\n";
    // Echo of the setpoint register read back from the PLC.
    o << "[[datapoint]]\nid = \"setpoint_echo\"\nkind = \"Status\"\ntype = \"U16\"\n"
      << "source = { port = \"plc\", table = \"HR\", addr = " << kSetpointAddr << " }\n\n";

    // Protocol-conversion bridge: operator box (server) <-> PLC (client).
    //  mirror PLC HR[0,8) -> opbox table   (operator box sees live data)
    //  forward opbox HR[20,22) -> PLC       (operator commands reach the PLC)
    o << "[[bridge]]\nserver = \"opbox\"\nplc = \"plc\"\noffset = 0\n"
      << "write_start = " << kSetpointAddr << "\nwrite_count = 2\n"
      << "mirror_start = 0\nmirror_count = 8\nmirror_period_ms = 200\n";

    return o.str();
}

bool GatewayController::apply() {
    stop();

    auto const toml = buildToml();
    QString const path = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("modbus_hmi_%1.toml").arg(m_plcPort));
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            setStatus(QStringLiteral("无法写入临时配置: %1").arg(path));
            return false;
        }
        f.write(QByteArray::fromStdString(toml));
    }

    m_core = core::ICore::create();
    if (auto r = m_core->loadConfig(path.toStdString()); !r.has_value()) {
        QString msg;
        for (auto const& e : r.error())
            msg += QStringLiteral("[%1.%2] %3\n")
                       .arg(QString::fromStdString(e.section),
                            QString::fromStdString(e.field),
                            QString::fromStdString(e.message));
        m_core.reset();
        setStatus(QStringLiteral("配置错误:\n%1").arg(msg));
        emit runningChanged();
        return false;
    }
    if (m_logSink) m_core->logger().addSink(m_logSink);
    m_core->setServerForwardEnabled("opbox", m_forwarding);
    m_core->start();
    m_timer.start();
    setStatus(QStringLiteral("运行中:PLC %1:%2,操作箱端口 %3,共 %4 个点")
                  .arg(m_host).arg(m_plcPort).arg(m_opboxPort).arg(m_config.size()));
    emit runningChanged();
    emit configChanged();
    return true;
}

void GatewayController::stop() {
    m_timer.stop();
    if (m_opbox) {
        m_opbox->disconnectDevice();
        m_opbox->deleteLater();
        m_opbox = nullptr;
    }
    if (m_core) {
        m_core->stop();
        m_core.reset();
        emit runningChanged();
    }
}

void GatewayController::writeSetpoint(int value) {
    if (!m_core) return;
    auto dp = m_core->datapoints().find("setpoint");
    if (!dp) return;
    dp->write(core::dp::makeValue(std::int64_t(value)));
    setStatus(QStringLiteral("下发设定值 %1 → PLC(经 Core sink)").arg(value));
}

void GatewayController::ensureOpboxClient() {
    if (m_opbox) return;
    m_opbox = new QModbusTcpClient(this);
    m_opbox->setConnectionParameter(QModbusDevice::NetworkAddressParameter,
                                    QStringLiteral("127.0.0.1"));
    m_opbox->setConnectionParameter(QModbusDevice::NetworkPortParameter, m_opboxPort);
    m_opbox->setTimeout(500);
    m_opbox->setNumberOfRetries(0);
}

void GatewayController::simulateOperatorWrite(int value) {
    ensureOpboxClient();
    auto send = [this, value]() {
        QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, kSetpointAddr, 1);
        unit.setValue(0, quint16(value));
        if (auto* reply = m_opbox->sendWriteRequest(unit, 1)) {
            if (!reply->isFinished())
                connect(reply, &QModbusReply::finished, reply, &QObject::deleteLater);
            else
                reply->deleteLater();
        }
    };
    if (m_opbox->state() == QModbusDevice::ConnectedState) {
        send();
    } else {
        connect(m_opbox, &QModbusClient::stateChanged, this,
                [this, send](QModbusDevice::State s) {
                    if (s == QModbusDevice::ConnectedState) {
                        send();
                        m_opbox->disconnect(SIGNAL(stateChanged(QModbusDevice::State)));
                    }
                });
        m_opbox->connectDevice();
    }
    setStatus(QStringLiteral("模拟操作箱写入 HR%1 = %2(经 Modbus server → 桥接%3)")
                  .arg(kSetpointAddr).arg(value)
                  .arg(m_forwarding ? QStringLiteral(" → PLC") : QStringLiteral(",但转发关闭")));
}

void GatewayController::refresh() {
    if (!m_core) return;
    QVariantList list;
    bool anyValid = false;
    auto& reg = m_core->datapoints();

    auto rowFor = [&](QString const& id, int address, QString const& type,
                      double scale, bool isEcho) -> QVariantMap {
        QVariantMap m;
        m[QStringLiteral("id")]      = id;
        m[QStringLiteral("address")] = address;
        m[QStringLiteral("type")]    = type;
        m[QStringLiteral("scale")]   = scale;
        m[QStringLiteral("role")]    = isEcho ? QStringLiteral("echo") : QStringLiteral("status");
        if (auto dp = reg.find(id.toStdString())) {
            m[QStringLiteral("value")] = core::dp::toQVariant(dp->value());
            m[QStringLiteral("state")] = QString::fromStdString(dp->stateText());
            m[QStringLiteral("valid")] = dp->valid();
            anyValid = anyValid || dp->valid();
        } else {
            m[QStringLiteral("value")] = QStringLiteral("—");
            m[QStringLiteral("state")] = QStringLiteral("Missing");
            m[QStringLiteral("valid")] = false;
        }
        return m;
    };

    for (auto const& p : m_config)
        list.append(rowFor(p.id, p.address, p.type, p.scale, false));
    list.append(rowFor(QStringLiteral("setpoint_echo"), kSetpointAddr,
                       QStringLiteral("U16"), 1.0, true));

    m_points = std::move(list);
    m_connectedCache = anyValid;
    emit tick();
}

bool GatewayController::connected() const { return m_connectedCache; }

void GatewayController::setStatus(QString s) {
    m_status = std::move(s);
    emit statusChanged();
}

void GatewayController::appendLog(QVariantMap record) {
    m_logs.prepend(record);
    while (m_logs.size() > kMaxLogs) m_logs.removeLast();
    emit logsChanged();
}

bool GatewayController::selfTest() {
    // Bring the runtime up; main drives the event loop and inspects results.
    return apply();
}
