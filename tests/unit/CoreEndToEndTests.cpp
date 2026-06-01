#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QTemporaryFile>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/codec/Codec.h"
#include "core/codec/CodecRegistry.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/internal/Testing.h"
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
    REQUIRE(core->transport("tcp1")->state() == transport::ConnectionState::Connected);

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

TEST_CASE("ICore registers builtin codecs at construction",
          "[core][codec]") {
    auto core = ICore::create(nullptr);
    REQUIRE(core->codecs().find("builtin.u16") != nullptr);
    REQUIRE(core->codecs().find("builtin.f32") != nullptr);
    REQUIRE(core->codecs().find("builtin.bool") != nullptr);
}
