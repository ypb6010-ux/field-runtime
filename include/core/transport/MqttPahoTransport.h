// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// MqttPahoTransport — MQTT client backed by Eclipse `paho.mqtt.cpp`. Has the
// same surface as `MqttClientTransport` (Qt6::Mqtt); the only operational
// differences:
//   - LICENSE: EPL 2.0 (Eclipse) instead of LGPLv3 (Qt Mqtt). Friendlier
//     for statically-linked closed-source industrial deployments.
//   - THREADING: paho runs its own dispatcher thread (no Qt event loop
//     dependency), so this transport works in non-GUI contexts more
//     cleanly.
//   - TLS: paho integrates with OpenSSL directly; Qt Mqtt rides on Qt's
//     SSL stack.
//
// Pick one or the other per transport via TOML `kind = mqtt_qt_client`
// vs `kind = mqtt_paho_client`.
class CORE_EXPORT MqttPahoTransport : public Transport {
public:
    struct Config {
        QString  id          = QStringLiteral("mqtt-paho-1");
        QString  brokerUri   = QStringLiteral("tcp://127.0.0.1:1883");
        QString  clientId    = QStringLiteral("core-paho-client");
        QString  username;
        QString  password;
        QString  topicPrefix;             // e.g. "factory/zone1/"
        QString  topicTemplate = QStringLiteral("reg/%1");
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

    explicit MqttPahoTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~MqttPahoTransport() override;

    CORE_DISABLE_COPY_MOVE(MqttPahoTransport)

    QString               id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;
    TransportStatus       status() const override;

    std::expected<void, QString> connect()    override;
    void                          disconnect() override;

    sched::RequestScheduler& scheduler() override;

    ReadResult  read      (ReadRequest const& req)         override;
    WriteResult writeBatch(WriteBatch  const& batch)       override;
    void        writeAsync(WriteBatch const& batch, WriteDone done) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
