#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QTemporaryFile>

#include "core/config/ConfigLoader.h"
#include "core/dp/ScalarType.h"

using namespace core::config;
using core::dp::ScalarType;

namespace {

// Drop a TOML snippet to a temp file and return its path. The QTemporaryFile
// outlives the test scope only because Catch2 RAII keeps the lambda block.
QString writeTomlFile(QString const& contents, QTemporaryFile& f) {
    REQUIRE(f.open());
    f.write(contents.toUtf8());
    f.flush();
    return f.fileName();
}

} // namespace

TEST_CASE("ConfigLoader parses a minimal transport + datapoint config",
          "[config][parse]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[meta]
project = "demo"
version = "0.1"

[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = 51500
slave_id = 1

[transport.scheduler]
kind                 = "serial"
inter_request_gap_ms = 5
max_queue_depth      = 64

[[poll_range]]
module_id = "poll.tcp1.hr"
transport = "tcp1"
table     = "HR"
range     = [0, 4]
period_ms = 200
priority  = "Normal"

[[datapoint]]
id   = "a"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0 }

[[datapoint]]
id   = "b"
kind = "Status"
type = "S16"
source = { port="tcp1", table="HR", addr=2, scale=0.1, offset=-40.0 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE(result.has_value());

    auto const& s = result.value();
    REQUIRE(s.meta.project == "demo");
    REQUIRE(s.transports.size() == 1);
    REQUIRE(s.transports.first().id == "tcp1");
    REQUIRE(s.transports.first().port == 51500);
    REQUIRE(s.transports.first().scheduler.interRequestGapMs == 5);
    REQUIRE(s.transports.first().scheduler.maxQueueDepth == 64);

    REQUIRE(s.pollRanges.size() == 1);
    REQUIRE(s.pollRanges.first().moduleId == "poll.tcp1.hr");
    REQUIRE(s.pollRanges.first().startAddress == 0);
    REQUIRE(s.pollRanges.first().count == 4);
    REQUIRE(s.pollRanges.first().periodMs == 200);

    REQUIRE(s.datapoints.size() == 2);
    REQUIRE(s.datapoints[0].id == "a");
    REQUIRE(s.datapoints[0].type == ScalarType::U16);
    REQUIRE(s.datapoints[0].source.port == "tcp1");
    REQUIRE(s.datapoints[0].source.address == 0);
    REQUIRE(s.datapoints[1].source.scale == 0.1);
    REQUIRE(s.datapoints[1].source.offset == -40.0);
}

TEST_CASE("ConfigLoader rejects duplicate transport ids",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id    = "dup"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = 502

[[transport]]
id    = "dup"
kind  = "modbus_tcp_client"
host  = "127.0.0.2"
port  = 502
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.section == "transport" && e.field == "id"
         && e.message.contains("duplicate id")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects datapoint referencing unknown transport",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"

[[datapoint]]
id   = "stray"
kind = "Status"
type = "U16"
source = { port="ghost", table="HR", addr=0 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "port" && e.message.contains("ghost")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects 32-bit datapoint without wordOrder",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"

[[datapoint]]
id   = "speed"
kind = "Status"
type = "F32"
source = { port="tcp1", table="HR", addr=0x100 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "wordOrder") { found = true; break; }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects Bool datapoint without bit",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"

[[datapoint]]
id   = "flag"
kind = "Status"
type = "Bool"
source = { port="tcp1", table="HR", addr=10 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "bit") { found = true; break; }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader surfaces TOML parse errors with a line number",
          "[config][parse-error]") {
    QTemporaryFile temp;
    // unterminated string
    auto path = writeTomlFile(R"toml(
[meta]
project = "demo
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(!result.error().isEmpty());
    REQUIRE(result.error().first().field == "toml");
}

TEST_CASE("ConfigLoader parses enum_u16 codec map", "[config][codec]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[codec]]
id   = "belt_state"
kind = "enum_u16"
map  = { 0 = "Stopped", 1 = "Starting", 2 = "Running" }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE(result.has_value());
    REQUIRE(result.value().codecs.size() == 1);
    auto const& c = result.value().codecs.first();
    REQUIRE(c.id == "belt_state");
    REQUIRE(c.kind == "enum_u16");
    REQUIRE(c.map.size() == 3);
    REQUIRE(c.map.value("0").toString() == "Stopped");
    REQUIRE(c.map.value("2").toString() == "Running");
}
