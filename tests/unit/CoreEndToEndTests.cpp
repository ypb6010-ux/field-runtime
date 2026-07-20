// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QTemporaryFile>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/codec/Codec.h"
#include "core/codec/CodecRegistry.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/internal/Testing.h"
#include "core/module/ModuleRegistry.h"
#include "core/module/SinkWindow.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/Transport.h"
#include "mocks/ModbusTestServer.h"

using namespace core;
using Catch::Matchers::WithinAbs;

namespace {

// Use a per-test port to avoid contention if Catch2 ever parallelises.
quint16 nextE2EPort() {
    static quint16 port = 51600;
    return ++port;
}

QString writeToml(QString const& contents, QTemporaryFile& f) {
    REQUIRE(f.open());
    f.write(contents.toUtf8());
    f.flush();
    return f.fileName();
}

// ICore::start() now connects transports in parallel without blocking, so the
// connection completes shortly AFTER start() returns. Poll for it.
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

} // namespace

TEST_CASE("ICore loads a TOML config and polls a real Modbus server end-to-end",
          "[core][e2e]") {
    auto port = nextE2EPort();

    core::test::ModbusTestServer srv(port);
    REQUIRE(srv.listening());
    srv.setData(QModbusDataUnit::HoldingRegisters, 0, 0x0042);   // U16 value
    srv.setData(QModbusDataUnit::HoldingRegisters, 2, 600);      // S16 raw
    // F32 23.5 in CDAB lays out as [0x0000, 0x41BC]
    srv.setData(QModbusDataUnit::HoldingRegisters, 4, 0x0000);
    srv.setData(QModbusDataUnit::HoldingRegisters, 5, 0x41BC);

    QTemporaryFile cfg;
    auto path = writeToml(QString(R"toml(
[meta]
project = "e2e"
version = "0.1"

[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = %1
slave_id = 1
connect_timeout_ms = 500

[transport.scheduler]
kind                 = "serial"
inter_request_gap_ms = 0

[[poll_range]]
module_id = "poll.tcp1.hr"
transport = "tcp1"
table     = "HR"
range     = [0, 8]
period_ms = 200

[[datapoint]]
id   = "raw_u16"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0 }

[[datapoint]]
id   = "temp"
kind = "Status"
type = "S16"
source = { port="tcp1", table="HR", addr=2, scale=0.1, offset=-40.0 }

[[datapoint]]
id   = "speed"
kind = "Status"
type = "F32"
source = { port="tcp1", table="HR", addr=4, wordOrder="CDAB" }
)toml").arg(port), cfg);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE(loaded.has_value());
    REQUIRE(core->transport("tcp1") != nullptr);
    REQUIRE(core->datapoints().find("raw_u16") != nullptr);
    REQUIRE(core->datapoints().find("temp")    != nullptr);
    REQUIRE(core->datapoints().find("speed")   != nullptr);

    bool ready = false;
    auto sub = core->bus().subscribe<bus::CoreReady>([&](auto const&) { ready = true; });

    core->start();
    REQUIRE(ready);
    REQUIRE(waitConnected(*core, "tcp1"));

    internal::pollAllOnce(*core);

    auto u16  = core->datapoints().find("raw_u16");
    auto temp = core->datapoints().find("temp");
    auto sp   = core->datapoints().find("speed");
    REQUIRE(u16->valid());
    REQUIRE(temp->valid());
    REQUIRE(sp->valid());
    REQUIRE(u16->value().value<quint16>() == 0x42);
    // raw 600 * 0.1 + (-40) = 20.0
    REQUIRE_THAT(temp->value().toDouble(), WithinAbs(20.0, 1e-6));
    REQUIRE_THAT(sp->value().toDouble(),   WithinAbs(23.5, 1e-6));

    core->stop();
}

TEST_CASE("ICore.loadConfig returns ValidationErrors for malformed input",
          "[core][e2e][error]") {
    QTemporaryFile cfg;
    auto path = writeToml(R"toml(
[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"

[[datapoint]]
id   = "orphan"
kind = "Status"
type = "U16"
source = { port="nope", table="HR", addr=0 }
)toml", cfg);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE_FALSE(loaded.error().isEmpty());
}

TEST_CASE("ICore rejects a second configuration load instead of mixing graphs",
          "[core][config][lifecycle]") {
    QTemporaryFile cfg;
    auto path = writeToml(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"
)toml", cfg);

    auto core = ICore::create(nullptr);
    REQUIRE(core->loadConfig(path).has_value());

    auto second = core->loadConfig(path);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().first().section == "core");
    REQUIRE(second.error().first().message.contains("already loaded"));
    REQUIRE(core->transportIds() == QStringList{"tcp1"});
}

TEST_CASE("ICore invalid reload leaves the running graph untouched",
          "[core][config][reload][rollback]") {
    auto const port = nextE2EPort();
    QTemporaryFile active;
    auto activePath = writeToml(QString(R"toml(
[[transport]]
id = "server.old"
kind = "modbus_tcp_server"
listen_address = "127.0.0.1"
listen_port = %1
[[transport.listen_ranges]]
table = "HR"
range = [0, 4]

[[datapoint]]
id = "old.dp"
kind = "Status"
type = "U16"
source = { port="server.old", table="HR", addr=0 }
)toml").arg(port), active);
    QTemporaryFile invalid;
    auto invalidPath = writeToml(QStringLiteral(R"toml(
[[transport]]
id = "broken"
kind = "modbus_tcp_client"
port = 70000
)toml"), invalid);

    auto core = ICore::create(nullptr);
    REQUIRE(core->loadConfig(activePath).has_value());
    core->start();
    REQUIRE(waitConnected(*core, QStringLiteral("server.old")));
    auto* const oldTransport = core->transport(QStringLiteral("server.old"));
    std::atomic<int> failedEvents{0};
    auto sub = core->bus().subscribe<bus::ConfigReloadFailed>(
        [&](bus::ConfigReloadFailed const&) { failedEvents.fetch_add(1); });

    auto reloaded = core->reloadConfig(invalidPath);
    REQUIRE_FALSE(reloaded.has_value());
    REQUIRE(core->transport(QStringLiteral("server.old")) == oldTransport);
    REQUIRE(core->datapoints().find(QStringLiteral("old.dp")) != nullptr);
    REQUIRE(core->serverForwardEnabled(QStringLiteral("server.old")));
    REQUIRE(core->transportStatus(QStringLiteral("server.old")).state
            == transport::ConnectionState::Connected);
    REQUIRE(failedEvents.load() == 1);
    core->stop();
}

TEST_CASE("ICore valid reload atomically replaces the graph and announces rebuild",
          "[core][config][reload][model]") {
    auto const oldPort = nextE2EPort();
    auto const newPort = nextE2EPort();
    QTemporaryFile oldConfig;
    QTemporaryFile newConfig;
    auto oldPath = writeToml(QString(R"toml(
[[transport]]
id = "server.old"
kind = "modbus_tcp_server"
listen_address = "127.0.0.1"
listen_port = %1
[[transport.listen_ranges]]
table = "HR"
range = [0, 4]
[[datapoint]]
id = "old.dp"
kind = "Status"
type = "U16"
source = { port="server.old", table="HR", addr=0 }
)toml").arg(oldPort), oldConfig);
    auto newPath = writeToml(QString(R"toml(
[[transport]]
id = "server.new"
kind = "modbus_tcp_server"
listen_address = "127.0.0.1"
listen_port = %1
[[transport.listen_ranges]]
table = "HR"
range = [10, 4]
[[datapoint]]
id = "new.dp"
kind = "Status"
type = "U16"
source = { port="server.new", table="HR", addr=10 }
)toml").arg(newPort), newConfig);

    auto core = ICore::create(nullptr);
    REQUIRE(core->loadConfig(oldPath).has_value());
    core->start();
    REQUIRE(waitConnected(*core, QStringLiteral("server.old")));
    std::atomic<int> succeeded{0};
    std::atomic<int> rebuilt{0};
    std::atomic<int> reentrantRejected{0};
    auto startedSub = core->bus().subscribe<bus::ConfigReloadStarted>(
        [&](bus::ConfigReloadStarted const&) {
            auto nested = core->reloadConfig(newPath);
            if (!nested.has_value()
                && nested.error().first().message.contains(
                    QStringLiteral("already in progress"))) {
                reentrantRejected.fetch_add(1);
            }
        });
    auto successSub = core->bus().subscribe<bus::ConfigReloadSucceeded>(
        [&](bus::ConfigReloadSucceeded const&) { succeeded.fetch_add(1); });
    auto rebuildSub = core->bus().subscribe<bus::DatapointModelRebuilt>(
        [&](bus::DatapointModelRebuilt const& event) {
            if (event.generation > 0) rebuilt.fetch_add(1);
        });

    REQUIRE(core->reloadConfig(newPath).has_value());
    REQUIRE(waitConnected(*core, QStringLiteral("server.new")));
    REQUIRE(core->transport(QStringLiteral("server.old")) == nullptr);
    REQUIRE(core->transport(QStringLiteral("server.new")) != nullptr);
    REQUIRE(core->datapoints().find(QStringLiteral("old.dp")) == nullptr);
    REQUIRE(core->datapoints().find(QStringLiteral("new.dp")) != nullptr);
    REQUIRE_FALSE(core->serverForwardEnabled(QStringLiteral("server.new")));
    REQUIRE(succeeded.load() == 1);
    REQUIRE(rebuilt.load() == 1);
    REQUIRE(reentrantRejected.load() == 1);
    REQUIRE(core->reloadConfig(oldPath).has_value());
    REQUIRE(waitConnected(*core, QStringLiteral("server.old")));
    REQUIRE_FALSE(core->serverForwardEnabled(QStringLiteral("server.old")));
    REQUIRE(succeeded.load() == 2);
    REQUIRE(rebuilt.load() == 2);
    REQUIRE(reentrantRejected.load() == 2);
    core->stop();
}

TEST_CASE("ICore rejects configuration loading after start",
          "[core][config][lifecycle]") {
    QTemporaryFile cfg;
    auto path = writeToml(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"
)toml", cfg);

    auto core = ICore::create(nullptr);
    core->start();
    auto loaded = core->loadConfig(path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().first().message.contains("while Core is running"));
    core->stop();
}

TEST_CASE("ICore reports an explicit codec load failure without fallback",
          "[core][config][codec]") {
    QTemporaryFile cfg;
    auto path = writeToml(R"toml(
[[codec]]
id     = "site_codec"
kind   = "lua"
script = "definitely-missing-codec.lua"
)toml", cfg);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().first().section == "codec[0]");
    REQUIRE(core->codecs().find("site_codec") == nullptr);
}

TEST_CASE("ICore treats a declared plugin load failure as a config error",
          "[core][config][plugin]") {
    QTemporaryFile cfg;
    auto path = writeToml(R"toml(
[[transport]]
id   = "would_be_partial"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[plugin]]
name = "missing"
dll  = "definitely-missing-plugin.dll"
)toml", cfg);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().first().section == "plugin[0]");
    REQUIRE(core->transportIds().isEmpty());   // no partial object graph
}

TEST_CASE("ICore registers builtin codecs at construction",
          "[core][codec]") {
    auto core = ICore::create(nullptr);
    REQUIRE(core->codecs().find("builtin.u16") != nullptr);
    REQUIRE(core->codecs().find("builtin.f32") != nullptr);
    REQUIRE(core->codecs().find("builtin.bool") != nullptr);
}

TEST_CASE("Operator-box write through server → SinkWindow → PLC client",
          "[core][e2e][server-flow]") {
    auto plcPort = nextE2EPort();
    auto boxPort = nextE2EPort();

    // PLC fixture — the destination for SinkWindow flushes. Need register
    // count above the SinkWindow's write window (addresses 100..103).
    core::test::ModbusTestServer plc(plcPort, /*slaveId=*/1, /*registers=*/256);
    REQUIRE(plc.listening());

    QTemporaryFile cfg;
    auto path = writeToml(QString(R"toml(
[meta]
project = "server-flow"

[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = %1
slave_id = 1
connect_timeout_ms = 500

[[transport]]
id   = "box1"
kind = "modbus_tcp_server"
listen_address = "127.0.0.1"
listen_port    = %2
slave_id       = 1
[[transport.listen_ranges]]
table = "HR"
range = [0, 32]

[[sink_window]]
module_id = "sink.tcp1.hr"
transport = "tcp1"
table     = "HR"
range     = [100, 4]
priority  = "High"
[sink_window.flush]
debounce_ms  = 10
keepalive_ms = 0

[[datapoint]]
id   = "cmd_in"
kind = "Command"
type = "U16"
source = { port="box1", table="HR", addr=0 }
sink   = { port="tcp1", table="HR", addr=100, window="sink.tcp1.hr" }

[[route]]
name   = "box-to-plc"
from   = "cmd_in"
to     = "cmd_in"
policy = "ContinuousMirror"
)toml").arg(plcPort).arg(boxPort), cfg);

    auto core = ICore::create(nullptr);
    auto loaded = core->loadConfig(path);
    REQUIRE(loaded.has_value());

    // Sanity probe — verify the SinkWindow router actually receives writes.
    std::atomic<int> serverWriteEvents{0};
    auto probe = core->bus().subscribe<bus::ServerWriteEvent>(
        [&](bus::ServerWriteEvent const& e) {
            if (e.transportId == "box1") serverWriteEvents.fetch_add(1);
        });

    core->start();
    REQUIRE(waitConnected(*core, "tcp1"));
    REQUIRE(waitConnected(*core, "box1"));

    // Operator box (acts as a Modbus client against our server transport).
    transport::ModbusTcpClientTransport::Config opCfg;
    opCfg.id   = "opbox";
    opCfg.host = "127.0.0.1";
    opCfg.port = boxPort;
    opCfg.slaveId = 1;
    opCfg.connectTimeoutMs = 500;
    opCfg.requestTimeoutMs = 500;
    transport::ModbusTcpClientTransport opbox(opCfg);
    REQUIRE(opbox.connect().has_value());

    auto w = opbox.writeBatch({QModbusDataUnit::HoldingRegisters, 0,
                                {0x1234}});
    REQUIRE(w.ok);

    // Give the server thread a beat to fire ServerWriteEvent → router → stage.
    auto deadlineEvt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadlineEvt
        && serverWriteEvents.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(serverWriteEvents.load() >= 1);

    // Snapshot inspection — verify the route handler staged into the window.
    auto* swMod = dynamic_cast<core::module::SinkWindow*>(
        core->modules().find("sink.tcp1.hr"));
    REQUIRE(swMod != nullptr);
    auto snap = swMod->snapshot();
    REQUIRE(snap.size() == 4);
    REQUIRE(snap[0] == 0x1234);

    // Trigger a sink-window tick so the staged value flushes to the PLC.
    internal::tickSinkWindowsOnce(*core);

    // Read back from the PLC fixture and verify.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    quint16 raw = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        raw = plc.getData(QModbusDataUnit::HoldingRegisters, 100);
        if (raw == 0x1234) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(raw == 0x1234);

    opbox.disconnect();
    core->stop();
}
