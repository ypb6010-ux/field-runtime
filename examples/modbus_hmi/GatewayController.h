// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace core { class ICore; }
namespace core::log { class ILogSink; }
class QModbusTcpClient;

// View-model that drives the Modbus HMI. It owns the Core runtime and rebuilds
// it from the UI's editable configuration (connection parameters + datapoint
// list). Everything QML needs is a property or a Q_INVOKABLE here, so the QML
// binds to this stable object while the underlying Core instance is swapped on
// Apply.
//
// Demonstrated capabilities:
//   1. Connection config  — host / PLC port / poll period, applied at runtime.
//   2. Datapoint config   — add / remove / edit Modbus points (addr/type/scale).
//   3. Live data display  — each point's decoded value + quality, refreshed.
//   4. Protocol conversion — a Modbus-server "operator box" bridged to the PLC;
//      a forwarding gate (control) and a simulated operator write that travels
//      operator-box → bridge → PLC and echoes back into a datapoint.
class GatewayController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString host        READ host        WRITE setHost        NOTIFY configChanged)
    Q_PROPERTY(int     plcPort     READ plcPort     WRITE setPlcPort     NOTIFY configChanged)
    Q_PROPERTY(int     opboxPort   READ opboxPort   WRITE setOpboxPort   NOTIFY configChanged)
    Q_PROPERTY(int     periodMs    READ periodMs    WRITE setPeriodMs    NOTIFY configChanged)
    Q_PROPERTY(bool    running     READ running     NOTIFY runningChanged)
    Q_PROPERTY(bool    connected   READ connected   NOTIFY tick)
    Q_PROPERTY(bool    forwarding  READ forwarding  WRITE setForwarding  NOTIFY forwardingChanged)
    Q_PROPERTY(QVariantList points READ points      NOTIFY tick)          // config + live
    Q_PROPERTY(QVariantList logs   READ logs        NOTIFY logsChanged)
    Q_PROPERTY(QString status      READ status      NOTIFY statusChanged)
public:
    explicit GatewayController(quint16 plcPort, quint16 opboxPort,
                               QObject* parent = nullptr);
    ~GatewayController() override;

    // Sink re-registered on the logger each time the Core is (re)built on apply.
    void setLogSink(std::shared_ptr<core::log::ILogSink> sink);

    QString host()      const { return m_host; }
    int     plcPort()   const { return m_plcPort; }
    int     opboxPort() const { return m_opboxPort; }
    int     periodMs()  const { return m_periodMs; }
    bool    running()   const { return m_core != nullptr; }
    bool    connected() const;
    bool    forwarding() const { return m_forwarding; }
    QVariantList points() const { return m_points; }
    QVariantList logs()   const { return m_logs; }
    QString status()    const { return m_status; }

    void setHost(QString v);
    void setPlcPort(int v);
    void setOpboxPort(int v);
    void setPeriodMs(int v);
    void setForwarding(bool v);

    // ── datapoint configuration ────────────────────────────────────────
    Q_INVOKABLE void addPoint(QString id, int address, QString type, double scale);
    Q_INVOKABLE void removePoint(QString id);

    // ── lifecycle ──────────────────────────────────────────────────────
    Q_INVOKABLE bool apply();          // (re)build the Core from current config
    Q_INVOKABLE void stop();

    // ── control / simulated operations ─────────────────────────────────
    Q_INVOKABLE void writeSetpoint(int value);          // downlink via Core sink
    Q_INVOKABLE void simulateOperatorWrite(int value);  // operator box → bridge → PLC

    // Headless self-test entry: drive the full flow and report, used by main.
    bool selfTest();

public slots:
    void appendLog(QVariantMap record);   // queued, from UiLogSink

signals:
    void configChanged();
    void runningChanged();
    void forwardingChanged();
    void tick();
    void logsChanged();
    void statusChanged();

private:
    struct PointCfg {
        QString id;
        int     address;
        QString type;     // "U16" / "S16" / "U32" / "F32" / "EnumU16" ...
        double  scale;
    };

    std::string buildToml() const;
    void        refresh();              // pull live values into m_points
    void        setStatus(QString s);
    void        ensureOpboxClient();    // lazy internal Modbus client for the opbox

    QString             m_host = QStringLiteral("127.0.0.1");
    int                 m_plcPort;
    int                 m_opboxPort;
    int                 m_periodMs = 500;
    bool                m_forwarding = true;
    QString             m_status;

    QList<PointCfg>     m_config;       // user-editable datapoint list
    QVariantList        m_points;       // config + live value/state (for QML)
    QVariantList        m_logs;         // newest first, capped

    std::unique_ptr<core::ICore> m_core;
    std::shared_ptr<core::log::ILogSink> m_logSink;
    QTimer              m_timer;
    QModbusTcpClient*   m_opbox = nullptr;
    bool                m_connectedCache = false;
};
