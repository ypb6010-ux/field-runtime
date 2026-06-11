// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// MqttClientTransport — MQTT v3.1.1 client backed by `Qt6::Mqtt`. For
// SCADA-to-cloud telemetry bridges or operator-box messaging.
//
// Each Modbus-style ReadRequest / WriteBatch maps to a deterministic topic
// via `topicTemplate`, e.g. "factory/zone1/reg/%1". The transport keeps an
// internal cache of payloads received on its `topicPrefix + #` wildcard
// subscription; `read()` looks up the cache, `writeBatch()` publishes.
//
// A sister class `MqttPahoTransport` provides the same surface against
// `paho.mqtt.cpp` (Eclipse Public License) for deployments where Qt Mqtt's
// LGPLv3 terms are inconvenient.
class CORE_EXPORT MqttClientTransport : public Transport {
public:
    struct Config {
        QString  id          = QStringLiteral("mqtt-1");
        QString  brokerUri   = QStringLiteral("tcp://127.0.0.1:1883");
        QString  clientId    = QStringLiteral("core-client");
        QString  username;
        QString  password;
        QString  topicPrefix;             // optional namespace e.g. "factory/zone1/"
        // Per-address topic template; the address is substituted into the
        // QString::arg placeholder, e.g. "factory/zone1/reg/%1".
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

    // read() is a non-blocking cache lookup, so the base readAsync default is
    // already non-blocking; only the publish path needs an async override.
    void writeAsync(WriteBatch const& batch, WriteDone done) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
