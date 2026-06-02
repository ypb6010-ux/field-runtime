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

TEST_CASE("ConfigLoader parses the full schema (sink_window / heartbeat / "
          "ack_watch / command / route)",
          "[config][schema-full]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id    = "tcp1"
kind  = "modbus_tcp_client"
host  = "127.0.0.1"
port  = 502
reconnect_interval_ms = 4000

[[transport]]
id   = "box1"
kind = "modbus_tcp_server"
listen_address = "0.0.0.0"
listen_port    = 5020
slave_id       = 1
[[transport.listen_ranges]]
table = "HR"
range = [0, 64]

[[poll_range]]
module_id = "poll.tcp1.hr"
transport = "tcp1"
table     = "HR"
range     = [0, 8]
period_ms = 100

[[sink_window]]
module_id = "sink.tcp1.hr"
transport = "tcp1"
table     = "HR"
range     = [100, 4]
priority  = "High"
initial   = [0, 0, 0, 0]
[sink_window.flush]
debounce_ms  = 30
keepalive_ms = 5000
coalesce     = true
max_retries  = 1

[[heartbeat]]
module_id = "hb.tcp1"
transport = "tcp1"
table     = "HR"
address   = 999
value     = 1
period_ms = 1000

[[ack_watch]]
module_id  = "ack.start"
dp         = "feedback"
expected   = 1
timeout_ms = 2000

[[command]]
module_id     = "cmd.start"
transport     = "tcp1"
priority      = "High"
interruptable = false
[[command.writes]]
table   = "HR"
address = 200
value   = 1

[[datapoint]]
id   = "feedback"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0 }

[[route]]
name   = "fwd"
from   = "feedback"
to     = "feedback"
policy = "ContinuousMirror"
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE(result.has_value());

    auto const& s = result.value();
    REQUIRE(s.transports.size() == 2);
    REQUIRE(s.transports[0].reconnectIntervalMs == 4000);
    REQUIRE(s.transports[1].listenRanges.size() == 1);
    REQUIRE(s.transports[1].listenRanges.first().size == 64);

    REQUIRE(s.sinkWindows.size() == 1);
    REQUIRE(s.sinkWindows.first().moduleId == "sink.tcp1.hr");
    REQUIRE(s.sinkWindows.first().startAddress == 100);
    REQUIRE(s.sinkWindows.first().size == 4);
    REQUIRE(s.sinkWindows.first().flush.debounceMs == 30);
    REQUIRE(s.sinkWindows.first().flush.keepaliveMs == 5000);
    REQUIRE(s.sinkWindows.first().initial.size() == 4);

    REQUIRE(s.heartbeats.size() == 1);
    REQUIRE(s.heartbeats.first().address == 999);
    REQUIRE(s.heartbeats.first().values.size() == 1);
    REQUIRE(s.heartbeats.first().values.first() == 1);

    REQUIRE(s.ackWatches.size() == 1);
    REQUIRE(s.ackWatches.first().dp == "feedback");
    REQUIRE(s.ackWatches.first().expected.toLongLong() == 1);

    REQUIRE(s.commands.size() == 1);
    REQUIRE(s.commands.first().writes.size() == 1);
    REQUIRE(s.commands.first().writes.first().address == 200);

    REQUIRE(s.routes.size() == 1);
    REQUIRE(s.routes.first().from == "feedback");
    REQUIRE(s.routes.first().to   == "feedback");
}

TEST_CASE("ConfigLoader rejects module_id collisions across sections",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[poll_range]]
module_id = "same"
transport = "tcp1"
table     = "HR"
range     = [0, 4]
period_ms = 100

[[heartbeat]]
module_id = "same"
transport = "tcp1"
address   = 50
value     = 1
period_ms = 1000
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "module_id" && e.message.contains("duplicate")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects route referencing unknown datapoint",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[datapoint]]
id   = "real"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0 }

[[route]]
from   = "ghost"
to     = "real"
policy = "ContinuousMirror"
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.section.startsWith("route") && e.field == "from"
         && e.message.contains("ghost")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects ack_watch referencing unknown datapoint",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[ack_watch]]
module_id = "ack.x"
dp        = "ghost"
expected  = 1
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.section.startsWith("ack_watch") && e.field == "dp") {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects sink_window with size > Modbus FC16 max (123)",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[sink_window]]
module_id = "sink.too.big"
transport = "tcp1"
table     = "HR"
range     = [0, 200]
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.section.startsWith("sink_window") && e.field == "range"
         && e.message.contains("FC16")) { found = true; break; }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects datapoint sink.addr outside its sink_window",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[sink_window]]
module_id = "sink.small"
transport = "tcp1"
table     = "HR"
range     = [100, 4]

[[datapoint]]
id   = "stray"
kind = "Command"
type = "U16"
sink = { port="tcp1", table="HR", addr=200, window="sink.small" }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "addr"
         && e.message.contains("outside sink_window")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects datapoint referencing unknown codec",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[datapoint]]
id   = "x"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0, codec="missing.codec" }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "codec" && e.message.contains("missing.codec")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects EnumU16 datapoint without explicit codec",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[datapoint]]
id   = "state"
kind = "Status"
type = "EnumU16"
source = { port="tcp1", table="HR", addr=0 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "codec" && e.message.contains("EnumU16")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader enforces kind ↔ source / sink consistency",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[datapoint]]
id   = "cmd_no_sink"
kind = "Command"
type = "U16"
source = { port="tcp1", table="HR", addr=0 }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "kind" && e.message.contains("Command")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects UntilAck policy without ack block",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[sink_window]]
module_id = "sw"
transport = "tcp1"
table     = "HR"
range     = [0, 4]

[[datapoint]]
id   = "needs_ack"
kind = "Command"
type = "U16"
sink   = { port="tcp1", table="HR", addr=0, window="sw" }
policy = "UntilAck"
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "policy" && e.message.contains("UntilAck")) {
            found = true; break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader rejects mask wider than the datapoint type",
          "[config][validate]") {
    QTemporaryFile temp;
    auto path = writeTomlFile(R"toml(
[[transport]]
id   = "tcp1"
kind = "modbus_tcp_client"
host = "127.0.0.1"

[[datapoint]]
id   = "wide_mask"
kind = "Status"
type = "U16"
source = { port="tcp1", table="HR", addr=0, mask=0xFFFFFF }
)toml", temp);

    ConfigLoader loader;
    auto result = loader.loadFromToml(path);
    REQUIRE_FALSE(result.has_value());
    bool found = false;
    for (auto const& e : result.error()) {
        if (e.field == "mask") { found = true; break; }
    }
    REQUIRE(found);
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
