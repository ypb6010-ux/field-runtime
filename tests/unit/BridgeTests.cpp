// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QTemporaryFile>

#include <chrono>
#include <thread>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/internal/Testing.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/Transport.h"
#include "core/transport/TransportTypes.h"
#include "mocks/ModbusTestServer.h"

using namespace core;
using namespace core::config;

namespace {

quint16 nextBridgePort() {
    static quint16 port = 51800;
    return ++port;
}

QString writeToml(QString const& contents, QTemporaryFile& f) {
    REQUIRE(f.open());
    f.write(contents.toUtf8());
    f.flush();
    return f.fileName();
}

bool waitConnected(ICore& core, QString const& id, int timeoutMs = 2000) {
    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto* t = core.transport(id);
        if (t && t->state() == transport::ConnectionState::Connected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Spin the poll pump until datapoint <id> reaches <expected> (async poll lands
// shortly after pollAllOnce returns).
bool waitDp(ICore& core, QString const& id, quint16 expected, int timeoutMs = 2000) {
    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        internal::pollAllOnce(core);
        auto dp = core.datapoints().find(id);
        if (dp && quint16(dp->value().toUInt()) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

QString bridgeToml(quint16 plcPort, quint16 serverPort) {
    return QStringLiteral(R"toml(
[meta]
project = "bridge"
version = "0.1"

[[transport]]
id    = "default"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = %1
slave_id = 1
[transport.scheduler]
kind = "serial"
inter_request_gap_ms = 2

[[transport]]
id    = "main"
kind  = "modbus_tcp_server"
listen_port = %2
slave_id = 1
max_clients = 1
[[transport.listen_ranges]]
table = "HR"
range = [0, 64]

[[poll_range]]
module_id = "poll.default"
transport = "default"
table     = "HR"
range     = [50, 4]
period_ms = 50

[[datapoint]]
id = "raw.default.HR.50"
kind = "Status"
type = "U16"
source = { port = "default", table = "HR", addr = 50 }
[[datapoint]]
id = "raw.default.HR.51"
kind = "Status"
type = "U16"
source = { port = "default", table = "HR", addr = 51 }

[[bridge]]
server = "main"
plc = "default"
offset = 0
write_start = 0
write_count = 4
mirror_start = 50
mirror_count = 4
mirror_period_ms = 50
)toml").arg(plcPort).arg(serverPort);
}

} // namespace

TEST_CASE("ConfigLoader rejects a bridge referencing an unknown transport",
          "[config][bridge]") {
    QTemporaryFile temp;
    auto path = writeToml(QStringLiteral(R"toml(
[meta]
project = "b"
version = "0.1"

[[transport]]
id   = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = 51999
slave_id = 1

[[bridge]]
server = "nope"
plc    = "plc"
mirror_start = 0
mirror_count = 0
)toml"), temp);

    ConfigLoader loader;
    auto schema = loader.loadFromToml(path);
    REQUIRE_FALSE(schema.has_value());
    bool flagged = false;
    for (auto const& e : schema.error()) {
        if (e.section.startsWith("bridge") && e.field == "server") flagged = true;
    }
    REQUIRE(flagged);
}

TEST_CASE("ConfigLoader flags a mirror range with no PLC datapoint",
          "[config][bridge]") {
    QTemporaryFile temp;
    auto path = writeToml(QStringLiteral(R"toml(
[meta]
project = "b"
version = "0.1"

[[transport]]
id   = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = 51999
slave_id = 1

[[transport]]
id   = "srv"
kind = "modbus_tcp_server"
listen_port = 51998
slave_id = 1

[[bridge]]
server = "srv"
plc    = "plc"
mirror_start = 50
mirror_count = 4
)toml"), temp);

    ConfigLoader loader;
    auto schema = loader.loadFromToml(path);
    REQUIRE_FALSE(schema.has_value());
    bool flagged = false;
    for (auto const& e : schema.error()) {
        if (e.section.startsWith("bridge") && e.field == "mirror") flagged = true;
    }
    REQUIRE(flagged);
}

TEST_CASE("Bridge mirrors PLC reads into the server table and forwards operator writes",
          "[core][bridge][e2e]") {
    auto const plcPort    = nextBridgePort();
    auto const serverPort = nextBridgePort();

    core::test::ModbusTestServer plc(plcPort);
    REQUIRE(plc.listening());
    plc.setData(core::RegisterTable::HoldingRegister, 50, 0x1234);
    plc.setData(core::RegisterTable::HoldingRegister, 51, 0x5678);

    QTemporaryFile temp;
    auto path = writeToml(bridgeToml(plcPort, serverPort), temp);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE(loaded.has_value());
    core->start();
    REQUIRE(waitConnected(*core, QStringLiteral("default")));

    // —— 镜像:PLC HR50/51 → server "main" 自己的表 ——
    REQUIRE(waitDp(*core, QStringLiteral("raw.default.HR.50"), 0x1234));
    REQUIRE(waitDp(*core, QStringLiteral("raw.default.HR.51"), 0x5678));

    auto* server = core->transport(QStringLiteral("main"));
    REQUIRE(server != nullptr);
    bool mirrored = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        internal::mirrorBridgesOnce(*core);
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport::ReadRequest req{core::RegisterTable::HoldingRegister, 50, 2};
        auto res = server->read(req);
        if (res.ok && res.values.size() == 2
         && res.values[0] == 0x1234 && res.values[1] == 0x5678) { mirrored = true; break; }
    }
    REQUIRE(mirrored);

    // —— 转发:真实操作箱(client)写 main HR0/1 → 真实 ServerWriteEvent → 转发到 PLC ——
    transport::ModbusTcpClientTransport::Config opCfg;
    opCfg.id = QStringLiteral("opbox");
    opCfg.host = QStringLiteral("127.0.0.1");
    opCfg.port = serverPort;
    opCfg.slaveId = 1;
    opCfg.connectTimeoutMs = 1000;
    opCfg.requestTimeoutMs = 1000;
    transport::ModbusTcpClientTransport opbox(opCfg);
    REQUIRE(opbox.connect().has_value());

    std::atomic<int> swCount{0};
    auto swSub = core->bus().subscribe<bus::ServerWriteEvent>(
        [&swCount](bus::ServerWriteEvent const&) { swCount.fetch_add(1); });

    auto w = opbox.writeBatch({core::RegisterTable::HoldingRegister, 0,
                               QList<quint16>{0xABCD, 0x0F0F}});
    REQUIRE(w.ok);

    bool forwarded = false;
    auto const fdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < fdl) {
        internal::tickSinkWindowsOnce(*core);   // 刷 bridge 转发 SinkWindow → PLC
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (plc.getData(core::RegisterTable::HoldingRegister, 0) == 0xABCD
         && plc.getData(core::RegisterTable::HoldingRegister, 1) == 0x0F0F) { forwarded = true; break; }
    }
    INFO("serverWriteEvents=" << swCount.load());
    REQUIRE(forwarded);

    opbox.disconnect();
    core->stop();
}
