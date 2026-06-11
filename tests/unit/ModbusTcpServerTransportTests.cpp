// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/ModbusTcpServerTransport.h"

using namespace core;
using namespace std::chrono_literals;

namespace {

quint16 nextServerPort() {
    static quint16 port = 51700;
    return ++port;
}

transport::ModbusTcpServerTransport::Config serverCfg(quint16 port) {
    transport::ModbusTcpServerTransport::Config c;
    c.id            = "server.test";
    c.listenAddress = "127.0.0.1";
    c.listenPort    = port;
    c.slaveId       = 1;
    c.listenRanges  = {
        {core::RegisterTable::HoldingRegister, 0, 100},
        {core::RegisterTable::InputRegister,   0, 50},
    };
    return c;
}

transport::ModbusTcpClientTransport::Config clientCfg(quint16 port) {
    transport::ModbusTcpClientTransport::Config c;
    c.id   = "client.test";
    c.host = "127.0.0.1";
    c.port = port;
    c.slaveId = 1;
    c.connectTimeoutMs = 500;
    c.requestTimeoutMs = 500;
    return c;
}

} // namespace

TEST_CASE("ModbusTcpServerTransport listens and exposes the configured table",
          "[server][listen]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    transport::ModbusTcpServerTransport srv(serverCfg(port), bus);

    REQUIRE(srv.id()    == "server.test");
    REQUIRE(srv.kind()  == transport::TransportKind::ModbusTcpServer);
    REQUIRE(srv.state() == transport::ConnectionState::Disconnected);

    auto opened = srv.connect();
    REQUIRE(opened.has_value());
    REQUIRE(srv.state() == transport::ConnectionState::Connected);

    srv.disconnect();
}

TEST_CASE("Server writeBatch updates the table observable by clients",
          "[server][write-self]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    transport::ModbusTcpServerTransport srv(serverCfg(port), bus);
    REQUIRE(srv.connect().has_value());

    auto w = srv.writeBatch({core::RegisterTable::HoldingRegister, 10,
                              {0x1111, 0x2222, 0x3333}});
    REQUIRE(w.ok);

    transport::ModbusTcpClientTransport client(clientCfg(port));
    REQUIRE(client.connect().has_value());
    auto r = client.read({core::RegisterTable::HoldingRegister, 10, 3});
    REQUIRE(r.ok);
    REQUIRE(r.values == QList<quint16>{0x1111, 0x2222, 0x3333});
}

TEST_CASE("Server publishes ServerWriteEvent on dataWritten",
          "[server][event]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    transport::ModbusTcpServerTransport srv(serverCfg(port), bus);
    REQUIRE(srv.connect().has_value());

    std::atomic<int>         eventCount{0};
    bus::ServerWriteEvent    captured{};
    std::mutex               capMtx;
    auto sub = bus.subscribe<bus::ServerWriteEvent>(
        [&](bus::ServerWriteEvent const& e) {
            std::lock_guard lk(capMtx);
            captured = e;
            eventCount.fetch_add(1);
        });

    transport::ModbusTcpClientTransport client(clientCfg(port));
    REQUIRE(client.connect().has_value());

    auto w = client.writeBatch({core::RegisterTable::HoldingRegister, 5,
                                  {0xAAAA, 0xBBBB}});
    REQUIRE(w.ok);

    // Allow time for the dataWritten signal to be dispatched.
    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (eventCount.load() < 1
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(eventCount.load() >= 1);

    std::lock_guard lk(capMtx);
    REQUIRE(captured.transportId == "server.test");
    REQUIRE(captured.startAddress == 5);
    REQUIRE(captured.values == QList<quint16>{0xAAAA, 0xBBBB});
    REQUIRE(captured.table == core::RegisterTable::HoldingRegister);
}

TEST_CASE("Server read returns its own current table values",
          "[server][read-self]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    transport::ModbusTcpServerTransport srv(serverCfg(port), bus);
    REQUIRE(srv.connect().has_value());

    srv.writeBatch({core::RegisterTable::HoldingRegister, 20, {0xDEAD, 0xBEEF}});
    auto r = srv.read({core::RegisterTable::HoldingRegister, 20, 2});
    REQUIRE(r.ok);
    REQUIRE(r.values == QList<quint16>{0xDEAD, 0xBEEF});
}
