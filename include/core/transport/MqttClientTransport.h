#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// MqttClientTransport — MQTT v3.1.1 / v5 client. For SCADA-to-cloud telemetry
// bridges. ReadRequest / WriteBatch map to subscribed-topic snapshots /
// published payloads; the QModbusDataUnit::RegisterType field is reused as a
// QoS hint (Coils=0, HoldingRegisters=1, InputRegisters=2).
//
// Placeholder until upstream `paho.mqtt.cpp` (v1.4.x, EPL) is vendored.
class CORE_EXPORT MqttClientTransport : public Transport {
public:
    struct Config {
        QString  id          = QStringLiteral("mqtt-1");
        QString  brokerUri   = QStringLiteral("tcp://127.0.0.1:1883");
        QString  clientId    = QStringLiteral("core-client");
        QString  username;
        QString  password;
        QString  topicPrefix;             // optional namespace e.g. "factory/zone1/"
        int      qos          = 1;
        bool     cleanSession = true;
        int      connectTimeoutMs    = 5000;
        int      requestTimeoutMs    = 2000;
        int      reconnectIntervalMs = 5000;
        sched::SchedulerConfig scheduler = []{
            sched::SchedulerConfig s;
            s.kind        = sched::SchedulerKind::Credit;
            s.maxInflight = 16;
            return s;
        }();
    };

    explicit MqttClientTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~MqttClientTransport() override;

    CORE_DISABLE_COPY_MOVE(MqttClientTransport)

    QString               id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;

    std::expected<void, QString> connect()    override;
    void                          disconnect() override;

    sched::RequestScheduler& scheduler() override;

    ReadResult  read      (ReadRequest const& req)         override;
    WriteResult writeBatch(WriteBatch  const& batch)       override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
