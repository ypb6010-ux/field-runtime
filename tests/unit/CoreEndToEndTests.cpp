// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QTemporaryFile>

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
        auto* t = core.transport(id.toStdString());
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
    srv.setData(core::RegisterTable::HoldingRegister, 0, 0x0042);   // U16 value
    srv.setData(core::RegisterTable::HoldingRegister, 2, 600);      // S16 raw
    // F32 23.5 in CDAB lays out as [0x0000, 0x41BC]
    srv.setData(core::RegisterTable::HoldingRegister, 4, 0x0000);
    srv.setData(core::RegisterTable::HoldingRegister, 5, 0x41BC);

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

    auto core = ICore::create();
    auto loaded = core->loadConfig(path.toStdString());
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
    REQUIRE(core::dp::toUInt64(u16->value()) == 0x42);
    // raw 600 * 0.1 + (-40) = 20.0
    REQUIRE_THAT(core::dp::toDouble(temp->value()), WithinAbs(20.0, 1e-6));
    REQUIRE_THAT(core::dp::toDouble(sp->value()),   WithinAbs(23.5, 1e-6));

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

    auto core = ICore::create();
    auto loaded = core->loadConfig(path.toStdString());
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE_FALSE(loaded.error().empty());
}

TEST_CASE("ICore registers builtin codecs at construction",
          "[core][codec]") {
    auto core = ICore::create();
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
coalesce     = true

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

    auto core = ICore::create();
    auto loaded = core->loadConfig(path.toStdString());
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

    auto w = opbox.writeBatch({core::RegisterTable::HoldingRegister, 0,
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
        raw = plc.getData(core::RegisterTable::HoldingRegister, 100);
        if (raw == 0x1234) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(raw == 0x1234);

    opbox.disconnect();
    core->stop();
}
