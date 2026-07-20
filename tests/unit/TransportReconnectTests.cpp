// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/bus/Subscription.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "mocks/ModbusTestServer.h"

using namespace core;
using namespace std::chrono_literals;

namespace {

quint16 nextReconnectPort() {
    static quint16 p = 51800;
    return ++p;
}

} // namespace

TEST_CASE("Client transport auto-reconnects after the server returns",
          "[transport][reconnect]") {
    auto const port = nextReconnectPort();

    bus::EventBus bus;
    std::atomic<int> connectedEvents{0};
    auto sub = bus.subscribe<bus::TransportStateChanged>(
        [&](bus::TransportStateChanged const& e) {
            if (e.after.state == transport::ConnectionState::Connected
                && e.before.state != transport::ConnectionState::Connected) {
                connectedEvents.fetch_add(1);
            }
        });

    transport::ModbusTcpClientTransport::Config cfg;
    cfg.id   = "tcp1";
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.slaveId = 1;
    cfg.connectTimeoutMs    = 500;
    cfg.requestTimeoutMs    = 500;
    cfg.reconnectIntervalMs = 100;
    transport::ModbusTcpClientTransport client(cfg, &bus);

    // No server yet — initial connect fails but auto-reconnect timer is armed.
    auto first = client.connect();
    REQUIRE_FALSE(first.has_value());

    // Start the server; auto-reconnect should pick it up within a few cycles.
    auto srv = std::make_unique<core::test::ModbusTestServer>(port);
    REQUIRE(srv->listening());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
        && client.state() != transport::ConnectionState::Connected) {
        // Process events so QTimer-driven reconnect can tick on the worker
        // thread. The reconnect timer lives on the client's QThread, which
        // has its own event loop; the worker spins independently of the
        // test thread.
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(client.state() == transport::ConnectionState::Connected);
    REQUIRE(connectedEvents.load() >= 1);

    client.disconnect();
}

TEST_CASE("Client transport reconnects after the server drops out",
          "[transport][reconnect][drop]") {
    auto const port = nextReconnectPort();

    bus::EventBus bus;
    std::atomic<int> connects{0}, disconnects{0};
    auto sub = bus.subscribe<bus::TransportStateChanged>(
        [&](bus::TransportStateChanged const& e) {
            if (e.after.state == transport::ConnectionState::Connected
                && e.before.state != transport::ConnectionState::Connected) {
                connects.fetch_add(1);
            }
            if (e.before.state == transport::ConnectionState::Connected
                && e.after.state != transport::ConnectionState::Connected) {
                disconnects.fetch_add(1);
            }
        });

    auto srv = std::make_unique<core::test::ModbusTestServer>(port);
    REQUIRE(srv->listening());

    transport::ModbusTcpClientTransport::Config cfg;
    cfg.id   = "tcp1";
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.slaveId = 1;
    cfg.connectTimeoutMs    = 500;
    cfg.requestTimeoutMs    = 500;
    cfg.reconnectIntervalMs = 100;
    transport::ModbusTcpClientTransport client(cfg, &bus);

    REQUIRE(client.connect().has_value());
    REQUIRE(connects.load() >= 1);

    // Drop the server.
    srv.reset();
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
        && client.state() == transport::ConnectionState::Connected) {
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(client.state() != transport::ConnectionState::Connected);

    // Bring it back.
    int const baseline = connects.load();
    srv = std::make_unique<core::test::ModbusTestServer>(port);
    REQUIRE(srv->listening());

    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
        && client.state() != transport::ConnectionState::Connected) {
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(client.state() == transport::ConnectionState::Connected);
    REQUIRE(connects.load() >= baseline + 1);

    client.disconnect();
}
