// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QtGlobal>   // quint16 — no longer transitively via the now Qt-free EventBus.h

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    auto const initial = srv.status();
    REQUIRE(initial.localEndpoint.address == "127.0.0.1");
    REQUIRE(initial.localEndpoint.port == port);

    auto opened = srv.connect();
    REQUIRE(opened.has_value());
    REQUIRE(srv.state() == transport::ConnectionState::Connected);
    REQUIRE(srv.status().revision > initial.revision);

    srv.disconnect();
    auto const closed = srv.status();
    srv.disconnect();
    REQUIRE(srv.status().revision == closed.revision);
    REQUIRE(srv.status().changedAt == closed.changedAt);
}

TEST_CASE("An occupied listen port produces a revisioned error snapshot",
          "[server][listen][error]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    transport::ModbusTcpServerTransport owner(serverCfg(port), bus);
    REQUIRE(owner.connect().has_value());

    auto contenderConfig = serverCfg(port);
    contenderConfig.id = "server.contender";
    transport::ModbusTcpServerTransport contender(contenderConfig, bus);
    std::vector<bus::TransportStateChanged> events;
    std::mutex eventMutex;
    auto sub = bus.subscribe<bus::TransportStateChanged>(
        [&](bus::TransportStateChanged const& event) {
            if (event.after.transportId != contenderConfig.id) return;
            std::lock_guard lock(eventMutex);
            events.push_back(event);
        });

    auto opened = contender.connect();
    REQUIRE_FALSE(opened.has_value());
    auto const status = contender.status();
    REQUIRE(status.state == transport::ConnectionState::Error);
    REQUIRE_FALSE(status.errorMessage.empty());
    REQUIRE(status.revision >= 2);
    std::lock_guard lock(eventMutex);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events.back().after == status);
}

TEST_CASE("Server tracks independent peer sessions and disconnects peers first",
          "[server][peer][lifecycle]") {
    auto port = nextServerPort();
    bus::EventBus bus;
    auto config = serverCfg(port);
    config.maxClients = 2;
    transport::ModbusTcpServerTransport srv(config, bus);
    std::vector<std::string> order;
    std::mutex orderMutex;
    auto peerSub = bus.subscribe<bus::PeerSessionChanged>(
        [&](bus::PeerSessionChanged const& event) {
            std::lock_guard lock(orderMutex);
            order.push_back(
                event.kind == bus::PeerSessionChangeKind::Connected
                ? "peer-up" : "peer-down");
        });
    auto stateSub = bus.subscribe<bus::TransportStateChanged>(
        [&](bus::TransportStateChanged const& event) {
            if (event.after.transportId != "server.test") return;
            std::lock_guard lock(orderMutex);
            order.push_back(
                event.after.state == transport::ConnectionState::Connected
                ? "listen-up" : "listen-down");
        });
    REQUIRE(srv.connect().has_value());

    auto cfg1 = clientCfg(port);
    cfg1.id = "client.one";
    auto cfg2 = clientCfg(port);
    cfg2.id = "client.two";
    transport::ModbusTcpClientTransport first(cfg1);
    transport::ModbusTcpClientTransport second(cfg2);
    REQUIRE(first.connect().has_value());
    REQUIRE(second.connect().has_value());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (srv.peerSessions().size() != 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    auto const sessions = srv.peerSessions();
    REQUIRE(sessions.size() == 2);
    REQUIRE(sessions[0].sessionId != sessions[1].sessionId);
    REQUIRE_FALSE(sessions[0].remoteEndpoint.address.empty());

    srv.disconnect();
    REQUIRE(srv.peerSessions().empty());
    std::lock_guard lock(orderMutex);
    auto const peerDown =
        std::find(order.rbegin(), order.rend(), "peer-down");
    auto const listenDown =
        std::find(order.rbegin(), order.rend(), "listen-down");
    REQUIRE(peerDown != order.rend());
    REQUIRE(listenDown != order.rend());
    REQUIRE(std::distance(order.rbegin(), peerDown)
            > std::distance(order.rbegin(), listenDown));
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
    REQUIRE(r.values == core::RegisterWords{0x1111, 0x2222, 0x3333});
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
    REQUIRE(captured.values == core::RegisterWords{0xAAAA, 0xBBBB});
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
    REQUIRE(r.values == core::RegisterWords{0xDEAD, 0xBEEF});
}
