// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/transport/MqttClientTransport.h"
#include "core/transport/MqttPahoTransport.h"

using namespace core::transport;

// ─── Qt6::Mqtt backend ─────────────────────────────────────────────

TEST_CASE("Qt Mqtt transport surfaces config id/kind/state",
          "[transport][mqtt][qt]") {
    MqttClientTransport::Config cfg;
    cfg.id        = "mqtt_qt";
    cfg.brokerUri = "tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 300;
    MqttClientTransport t(cfg);
    REQUIRE(t.id()   == "mqtt_qt");
    REQUIRE(t.kind() == TransportKind::MqttClient);
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("Qt Mqtt transport.connect to unreachable broker times out",
          "[transport][mqtt][qt]") {
    MqttClientTransport::Config cfg;
    cfg.id        = "mqtt_qt_bad";
    cfg.brokerUri = "tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 300;
    MqttClientTransport t(cfg);

    auto r = t.connect();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Qt Mqtt transport.read on disconnected returns 'not connected'",
          "[transport][mqtt][qt]") {
    MqttClientTransport::Config cfg;
    cfg.id        = "mqtt_qt_idle";
    cfg.brokerUri = "tcp://127.0.0.1:65535";
    MqttClientTransport t(cfg);

    auto rd = t.read({QModbusDataUnit::HoldingRegisters, 0, 2});
    REQUIRE_FALSE(rd.ok);
    REQUIRE(rd.errorMessage.contains("not connected"));
}

// ─── paho.mqtt.cpp backend ─────────────────────────────────────────

TEST_CASE("Paho Mqtt transport surfaces config id/kind/state",
          "[transport][mqtt][paho]") {
    MqttPahoTransport::Config cfg;
    cfg.id        = "mqtt_paho";
    cfg.brokerUri = "tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 300;
    MqttPahoTransport t(cfg);
    REQUIRE(t.id()   == "mqtt_paho");
    REQUIRE(t.kind() == TransportKind::MqttPahoClient);
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("Paho Mqtt transport.connect to unreachable broker fails",
          "[transport][mqtt][paho]") {
    MqttPahoTransport::Config cfg;
    cfg.id        = "mqtt_paho_bad";
    cfg.brokerUri = "tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 300;
    MqttPahoTransport t(cfg);

    auto r = t.connect();
    REQUIRE_FALSE(r.has_value());
}
