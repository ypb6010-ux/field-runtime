// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QTemporaryFile>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

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
id = "default.di.bit4"
kind = "Status"
type = "Bool"
source = { port = "default", table = "HR", addr = 50, bit = 4 }
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
mirror_policy = "AfterPoll"
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

TEST_CASE("ConfigLoader flags a mirror range with no covering PLC poll",
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
[[transport.listen_ranges]]
table = "HR"
range = [50, 4]

[[bridge]]
server = "srv"
plc    = "plc"
mirror_start = 50
mirror_count = 4
mirror_policy = "AfterPoll"
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

TEST_CASE("ConfigLoader rejects overlapping bridge command and mirror windows",
          "[config][bridge][stability]") {
    QTemporaryFile temp;
    auto path = writeToml(QStringLiteral(R"toml(
[meta]
project = "b"

[[transport]]
id = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = 51999

[[transport]]
id = "srv"
kind = "modbus_tcp_server"
listen_port = 51998
[[transport.listen_ranges]]
table = "HR"
range = [0, 16]

[[datapoint]]
id = "plc.hr0"
kind = "Status"
type = "U16"
source = { port = "plc", table = "HR", addr = 0 }

[[poll_range]]
module_id = "poll.plc"
transport = "plc"
table = "HR"
range = [0, 4]
period_ms = 50

[[bridge]]
server = "srv"
plc = "plc"
write_start = 0
write_count = 4
mirror_start = 0
mirror_count = 4
mirror_policy = "AfterPoll"
)toml"), temp);

    ConfigLoader loader;
    auto schema = loader.loadFromToml(path);
    REQUIRE_FALSE(schema.has_value());
    bool flagged = false;
    for (auto const& e : schema.error()) {
        if (e.section == "bridge[0]" && e.field == "range") flagged = true;
    }
    REQUIRE(flagged);
}

TEST_CASE("Bridge mirror policy is explicit and validated",
          "[config][bridge][policy]") {
    auto load = [](QString const& bridgeFields) {
        auto temp = std::make_unique<QTemporaryFile>();
        auto path = writeToml(QString(R"toml(
[[transport]]
id = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = 51999

[[transport]]
id = "srv"
kind = "modbus_tcp_server"
listen_port = 51998
[[transport.listen_ranges]]
table = "HR"
range = [50, 4]

[[poll_range]]
module_id = "poll.plc"
transport = "plc"
table = "HR"
range = [50, 4]
period_ms = 50

[[bridge]]
server = "srv"
plc = "plc"
mirror_start = 50
mirror_count = 4
%1
)toml").arg(bridgeFields), *temp);
        ConfigLoader loader;
        return std::pair{std::move(temp), loader.loadFromToml(path)};
    };

    auto [missingFile, missing] = load(QString{});
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(std::any_of(missing.error().cbegin(), missing.error().cend(),
        [](ValidationError const& error) {
            return error.field == QStringLiteral("mirror_policy");
        }));

    auto [periodFile, period] = load(QStringLiteral(
        "mirror_policy = \"Periodic\"\nmirror_period_ms = 100"));
    REQUIRE(period.has_value());
    REQUIRE(period->datapoints.isEmpty());
    REQUIRE(period->bridges.first().mirrorPolicy == BridgeMirrorPolicy::Periodic);

    auto [missingPeriodFile, missingPeriod] = load(
        QStringLiteral("mirror_policy = \"Periodic\""));
    REQUIRE_FALSE(missingPeriod.has_value());
    REQUIRE(std::any_of(missingPeriod.error().cbegin(),
                        missingPeriod.error().cend(),
        [](ValidationError const& error) {
            return error.field == QStringLiteral("mirror_period_ms");
        }));

    auto [badPeriodFile, badPeriod] = load(QStringLiteral(
        "mirror_policy = \"AfterPoll\"\nmirror_period_ms = 100"));
    REQUIRE_FALSE(badPeriod.has_value());
    REQUIRE(std::any_of(badPeriod.error().cbegin(), badPeriod.error().cend(),
        [](ValidationError const& error) {
            return error.field == QStringLiteral("mirror_period_ms");
        }));
}

TEST_CASE("Periodic bridge mirrors only the latest successful raw poll snapshot",
          "[core][bridge][periodic][raw]") {
    auto const plcPort = nextBridgePort();
    auto const serverPort = nextBridgePort();
    auto plc = std::make_unique<core::test::ModbusTestServer>(plcPort);
    REQUIRE(plc->listening());
    plc->setData(QModbusDataUnit::HoldingRegisters, 50, 0x1234);

    QTemporaryFile temp;
    auto path = writeToml(QString(R"toml(
[[transport]]
id = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = %1
connect_timeout_ms = 500

[[transport]]
id = "server"
kind = "modbus_tcp_server"
listen_address = "127.0.0.1"
listen_port = %2
[[transport.listen_ranges]]
table = "HR"
range = [50, 1]

[[poll_range]]
module_id = "poll.raw"
transport = "plc"
table = "HR"
range = [50, 1]
period_ms = 20

[[bridge]]
server = "server"
plc = "plc"
mirror_start = 50
mirror_count = 1
mirror_policy = "Periodic"
mirror_period_ms = 400
)toml").arg(plcPort).arg(serverPort), temp);

    auto core = ICore::create(nullptr);
    REQUIRE(core->loadConfig(path).has_value());
    std::atomic<int> completedPolls{0};
    auto pollSub = core->bus().subscribe<bus::PollRangeCompleted>(
        [&](bus::PollRangeCompleted const& event) {
            if (event.moduleId == QStringLiteral("poll.raw")) {
                completedPolls.fetch_add(1);
            }
        });
    auto* plcTransport = core->transport(QStringLiteral("plc"));
    auto* server = core->transport(QStringLiteral("server"));
    REQUIRE(plcTransport != nullptr);
    REQUIRE(server != nullptr);
    REQUIRE(plcTransport->connect().has_value());
    REQUIRE(server->connect().has_value());
    core->start();
    internal::pollAllOnce(*core);
    REQUIRE(completedPolls.load() > 0);

    // Periodic does not also run the AfterPoll path: immediately after a
    // successful manual poll, the server still holds its initial image.
    auto beforePeriod = server->read(
        {QModbusDataUnit::HoldingRegisters, 50, 1});
    REQUIRE(beforePeriod.ok);
    REQUIRE(beforePeriod.values == QList<quint16>{0});

    bool mirrored = false;
    auto const mirrorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!mirrored && std::chrono::steady_clock::now() < mirrorDeadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto result = server->read(
            {QModbusDataUnit::HoldingRegisters, 50, 1});
        mirrored = result.ok && result.values == QList<quint16>{0x1234};
    }
    REQUIRE(mirrored);

    // A failed poll never replaces the last valid raw image with zeroes.
    plc.reset();
    auto const settleDeadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(550);
    while (std::chrono::steady_clock::now() < settleDeadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto retained = server->read(
        {QModbusDataUnit::HoldingRegisters, 50, 1});
    REQUIRE(retained.ok);
    REQUIRE(retained.values == QList<quint16>{0x1234});
    core->stop();
}

TEST_CASE("Bridge mirrors PLC reads into the server table and forwards operator writes",
          "[core][bridge][e2e]") {
    auto const plcPort    = nextBridgePort();
    auto const serverPort = nextBridgePort();

    core::test::ModbusTestServer plc(plcPort);
    REQUIRE(plc.listening());
    plc.setData(QModbusDataUnit::HoldingRegisters, 50, 0x1234);
    plc.setData(QModbusDataUnit::HoldingRegisters, 51, 0x5678);

    QTemporaryFile temp;
    auto path = writeToml(bridgeToml(plcPort, serverPort), temp);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE(loaded.has_value());
    core->start();
    REQUIRE(waitConnected(*core, QStringLiteral("default")));

    // —— 镜像:PLC HR50/51 → server "main" 自己的表 ——
    REQUIRE(waitDp(*core, QStringLiteral("raw.default.HR.51"), 0x5678));

    auto* server = core->transport(QStringLiteral("main"));
    REQUIRE(server != nullptr);
    bool mirrored = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport::ReadRequest req{QModbusDataUnit::HoldingRegisters, 50, 2};
        auto res = server->read(req);
        if (res.ok && res.values.size() == 2
         && res.values[0] == 0x1234 && res.values[1] == 0x5678) { mirrored = true; break; }
    }
    REQUIRE(mirrored);

    // Disabling is a safety edge: even when the local forward snapshot has
    // never changed from its initial zeros, it must overwrite a non-zero PLC
    // value that may have come from another master or a prior runtime.
    plc.setData(QModbusDataUnit::HoldingRegisters, 0, 0x9999);
    core->setServerForwardEnabled(QStringLiteral("main"), false);
    bool neutralized = false;
    auto const ndl = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < ndl) {
        internal::tickSinkWindowsOnce(*core);
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (plc.getData(QModbusDataUnit::HoldingRegisters, 0) == 0) {
            neutralized = true;
            break;
        }
    }
    REQUIRE(neutralized);
    core->setServerForwardEnabled(QStringLiteral("main"), true);

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
    auto const peerDeadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(1);
    while (core->peerSessions(QStringLiteral("main")).size() != 1
           && std::chrono::steady_clock::now() < peerDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(core->peerSessions(QStringLiteral("main")).size() == 1);

    std::atomic<int> swCount{0};
    auto swSub = core->bus().subscribe<bus::ServerWriteEvent>(
        [&swCount](bus::ServerWriteEvent const&) { swCount.fetch_add(1); });

    auto w = opbox.writeBatch({QModbusDataUnit::HoldingRegisters, 0,
                               QList<quint16>{0xABCD, 0x0F0F}});
    REQUIRE(w.ok);

    bool forwarded = false;
    auto const fdl = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < fdl) {
        internal::tickSinkWindowsOnce(*core);   // 刷 bridge 转发 SinkWindow → PLC
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (plc.getData(QModbusDataUnit::HoldingRegisters, 0) == 0xABCD
         && plc.getData(QModbusDataUnit::HoldingRegisters, 1) == 0x0F0F) { forwarded = true; break; }
    }
    INFO("serverWriteEvents=" << swCount.load());
    REQUIRE(forwarded);

    opbox.disconnect();
    auto const peerGoneDeadline = std::chrono::steady_clock::now()
                                + std::chrono::seconds(1);
    while (!core->peerSessions(QStringLiteral("main")).isEmpty()
           && std::chrono::steady_clock::now() < peerGoneDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(core->peerSessions(QStringLiteral("main")).isEmpty());
    core->stop();
}
