# Core Handbook (English)

> Multi-protocol industrial runtime (Modbus / OPC UA / MQTT / S7) with typed
> datapoints, declarative routing, and half-duplex aware scheduling.

## Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Architecture](#3-architecture)
4. [TOML Configuration Reference](#4-toml-configuration-reference)
5. [Picking a Scheduler](#5-picking-a-scheduler)
6. [Multi-Protocol Transports](#6-multi-protocol-transports)
7. [Examples](#7-examples)
8. [Migration from Legacy Core](#8-migration-from-legacy-core)

---

## 1. Overview

Core is a runtime library for mining / industrial SCADA upper-computer
software. It distils the "PLC ↔ operator box ↔ HMI" stack into a small set
of crisply defined concepts:

- **Transport** — one physical connection (Modbus TCP/RTU, OPC UA, MQTT, S7)
- **Scheduler** — per-Transport request gatekeeper (Serial / Credit / Priority)
- **Datapoint** — a named logical signal with typed value + codec pipeline
- **Module** — a periodic or event-driven runtime unit (PollRange,
  SinkWindow, Heartbeat, Command, AckWatch)
- **EventBus** — type-erased publish/subscribe, thread-safe
- **ConfigLoader** — TOML parser + startup-time fail-fast validation

The library is C++23 + Qt 6.8, with dependencies on `async_simple`
(coroutines), `tomlplusplus` (parser), and `Catch2` (tests).

---

## 2. Quick Start

### 2.1 Build

Requires Qt 6.8, CMake 3.21, MSVC 19.36+ / GCC 13 / Clang 16:

```bash
cmake -S core -B core/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DQt6_DIR=$QT6_DIR
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

### 2.2 Three minimal examples

```bash
# 1) smallest — read 3 datapoints
example_minimal_modbus      examples/minimal_modbus/minimal.toml

# 2) operator-box → PLC bridge
example_operator_box_to_plc examples/operator_box_to_plc/bridge.toml

# 3) scheduler statistics dashboard
example_stats_dashboard     examples/stats_dashboard/dashboard.toml
```

See `core/examples/*/README.md` for details.

### 2.3 Integrating into your own application

```cpp
#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"

QCoreApplication app(argc, argv);
auto core = core::ICore::create(/*qmlContext*/ nullptr);

auto loaded = core->loadConfig("config.toml");
if (!loaded.has_value()) {
    for (auto const& e : loaded.error()) {
        qWarning() << "[config]" << e.section << e.field << e.message;
    }
    return 1;
}

// Subscribe to datapoint changes
auto sub = core->bus().subscribe<core::bus::DpChanged>(
    [](core::bus::DpChanged const& e) {
        qInfo() << e.id << "=" << e.value;
    });

core->start();
return app.exec();
```

---

## 3. Architecture

```
                        ┌──────────────────────────────┐
                        │           ICore               │
                        │  (facade — start/stop/load)   │
                        └─────┬───────────────┬─────────┘
                              │               │
              ┌───────────────▼──┐         ┌──▼────────────┐
              │  ModuleRegistry  │         │   EventBus    │
              │  (QTimer driver) │         │ (pub / sub)   │
              └─────┬────────────┘         └──┬────────────┘
                    │                         │
        ┌───────────┴──┬─────────────┐        │
        ▼              ▼             ▼        ▼
   ┌─────────┐  ┌────────────┐ ┌──────────┐
   │PollRange│  │ SinkWindow │ │Heartbeat │ ...
   └────┬────┘  └─────┬──────┘ └────┬─────┘
        │              │             │
        ▼              ▼             ▼
   ┌──────────────────────────────────────┐
   │             Transport                 │
   │  (Modbus TCP/RTU, OPC UA, MQTT, S7)   │
   │            ──────────                  │
   │           RequestScheduler             │
   │     (Serial / Credit / Priority)       │
   └──────────────────────────────────────┘
```

**Typical PLC-read data flow**:

```
PLC ──Modbus──► Transport.read
            ──► RequestScheduler.submit  (queue / throttle / breaker)
            ──► PollRange.pollOnce
            ──► Codec.decode (byte order + scale/offset/mask)
            ──► Datapoint.setValue
            ──► emit valueChanged
            ──► EventBus.publish(DpChanged)
            ──► QML / plugin / database subscribers see it
```

**Operator-box → PLC flow** (route table in `[[route]]`):

```
operator box ──Modbus write──► ServerTransport.dataWritten
                          ──► EventBus.publish(ServerWriteEvent)
                          ──► Core routes from → to
                          ──► SinkWindow.stageRegister
                          ──► QTimer periodic onTick → flush
                          ──► PLC client.writeBatch
                          ──► PLC
```

---

## 4. TOML Configuration Reference

The full schema is documented in
[`doc/design/Core-Greenfield-Spec.md` §4](../../../doc/design/Core-Greenfield-Spec.md#四-配置层-toml-schema-全集).
Below is the cheat sheet.

### 4.1 `[[transport]]`

```toml
[[transport]]
id   = "plc"                 # globally unique
kind = "modbus_tcp_client"   # or modbus_tcp_server / modbus_rtu / opc_ua_client / mqtt_client / s7_client

# Modbus TCP client / server / S7
host = "192.168.0.10"
port = 502
slave_id = 1
listen_address = "0.0.0.0"   # server only
listen_port    = 502         # server only

# Modbus RTU
port_name = "COM3"           # or /dev/ttyUSB0
baud_rate = 9600
data_bits = 8
stop_bits = 1
parity    = "none"           # none / even / odd

# OPC UA
endpoint_url    = "opc.tcp://host:4840"
security_policy = "None"
username        = ""
password        = ""

# MQTT
broker_uri    = "tcp://host:1883"
client_id     = "core-1"
topic_prefix  = "factory/zone1/"
qos           = 1
clean_session = true

# Common
reconnect_interval_ms = 15000   # 0 disables auto reconnect
connect_timeout_ms    = 3000
request_timeout_ms    = 1000

[transport.scheduler]
kind                       = "serial"   # serial / credit / priority
max_inflight               = 1          # credit / priority
inter_request_gap_ms       = 5          # 5–10 ms recommended for half-duplex
max_queue_depth            = 256
starvation_guard_ms        = 5000       # priority
fifo_within_lane           = true
circuit_breaker_threshold  = 10
circuit_breaker_open_ms    = 5000

# Server-mode transports declare their listen window:
[[transport.listen_ranges]]
table = "HR"
range = [0, 64]
```

### 4.2 `[[poll_range]]` / `[[sink_window]]` / `[[heartbeat]]` / `[[command]]` / `[[ack_watch]]`

```toml
# Periodic read
[[poll_range]]
module_id = "poll.plc.hr"
transport = "plc"
table     = "HR"
range     = [0, 16]        # [start_addr, register_count]
period_ms = 200
priority  = "Normal"       # Low / Normal / High / Critical

# Batched write window (Modbus FC16 cap = 123 registers)
[[sink_window]]
module_id = "sw.plc"
transport = "plc"
table     = "HR"
range     = [100, 4]       # [start_addr, size]
priority  = "High"
initial   = [0, 0, 0, 0]
[sink_window.flush]
debounce_ms  = 20          # how long after the last stage to flush
keepalive_ms = 5000        # even if nothing changed, refresh every N ms
coalesce     = true        # merge adjacent stages

# Heartbeat write
[[heartbeat]]
module_id = "hb.plc"
transport = "plc"
table     = "HR"
address   = 999
value     = 1              # or values = [1, 2, 3]
period_ms = 1000

# One-shot command
[[command]]
module_id = "cmd.start"
transport = "plc"
priority  = "High"
interruptable = false
[[command.writes]]
table = "HR"
address = 200
value = 1

# Ack watch
[[ack_watch]]
module_id  = "ack.start"
dp         = "feedback"
expected   = 1
timeout_ms = 2000
```

### 4.3 `[[datapoint]]` + `[[route]]`

```toml
[[datapoint]]
id   = "temperature"
kind = "Status"            # Status / Command / Bidirectional
type = "S16"               # Bool/U16/S16/U32/S32/F32/U64/S64/F64/EnumU16/String
source = { port="plc", table="HR", addr=0, scale=0.1, offset=-40.0 }

[[datapoint]]
id   = "start_button"
kind = "Bidirectional"
type = "Bool"
source = { port="box", table="HR", addr=10, bit=0 }
sink   = { port="plc", table="HR", addr=100, bit=0, window="sw.plc" }

[[route]]
name   = "start-button-fwd"
from   = "start_button"
to     = "start_button"
policy = "ContinuousMirror"  # ContinuousMirror / EdgeOnce / Pulsed / UntilAck
```

### 4.4 Startup validation

Loading goes through fail-fast validation; see spec §4.3. The most common
rules:

- IDs are globally unique across sections (transport / module_id / datapoint /
  codec)
- Every transport / datapoint / codec / sink_window reference must resolve
- 32-bit numeric types must declare `wordOrder`
- `Bool` must declare `bit`
- `EnumU16` must declare `codec`
- `sink_window.range[1] ≤ 123`
- `datapoint.sink.addr` must fall inside its referenced window
- `mask` may not exceed the type's bit width
- `policy=UntilAck` requires a `[datapoint.ack]` block

Errors carry both the field name and the source line:

```
[Config Error] datapoint[7] "belt2.cmd.start"
  field: source.wordOrder
  message: type=F32 requires wordOrder (ABCD/CDAB/BADC/DCBA), got null
  line: 142
```

---

## 5. Picking a Scheduler

| Scheduler   | When to use                                          | Key knobs |
|-------------|------------------------------------------------------|-----------|
| **Serial**  | Half-duplex devices (RS-485, Modbus RTU, shared TCP gateways) | `inter_request_gap_ms = 5~10` |
| **Credit**  | Full-duplex devices (OPC UA, MQTT, raw TCP) | `max_inflight = 4~16` |
| **Priority** | Emergency-stop scenarios (≤ 50 ms preemption) | `starvation_guard_ms = 5000`, `tag.interruptable = true` |

Behaviour at a glance:

- All schedulers have four priority lanes (Critical > High > Normal > Low);
  within each lane, `fifo_within_lane` picks FIFO or round-robin
- Credit fires up to `max_inflight` requests concurrently
- Priority promotes starved lanes once `starvation_guard_ms` elapses
- Any scheduler opens its circuit breaker after
  `circuit_breaker_threshold` consecutive errors and recovers via HalfOpen
  after `circuit_breaker_open_ms`

Subscribe to `bus::SchedulerStatsEvent` to receive per-transport snapshots
with queue depth, inflight count, p50/p99 latency, and circuit state.

---

## 6. Multi-Protocol Transports

### Fully implemented

| Protocol | Class | Dependency | TOML `kind` |
|----------|-------|------------|-------------|
| Modbus TCP client | `ModbusTcpClientTransport` | Qt6::SerialBus | `modbus_tcp_client` |
| Modbus TCP server | `ModbusTcpServerTransport` | Qt6::SerialBus | `modbus_tcp_server` |
| Modbus RTU       | `ModbusRtuTransport`       | Qt6::SerialBus + Qt6::SerialPort | `modbus_rtu` |
| OPC UA client    | `OpcUaClientTransport`     | **Qt6::OpcUa** (open62541 backend) | `opc_ua_client` |
| MQTT client (Qt) | `MqttClientTransport`      | **Qt6::Mqtt** | `mqtt_qt_client` or `mqtt_client` (alias) |
| MQTT client (paho) | `MqttPahoTransport`      | **paho.mqtt.cpp** (vcpkg) | `mqtt_paho_client` |

### Qt6::Mqtt vs paho.mqtt.cpp — picking a backend

Both MQTT backends expose **the same Transport interface**. They differ
only in the underlying library and its licensing:

| Dimension | `mqtt_qt_client` (Qt6::Mqtt) | `mqtt_paho_client` (paho.mqtt.cpp) |
|-----------|------------------------------|------------------------------------|
| **License** | LGPLv3 / Commercial (with Qt) | EPL 2.0 + EDL 1.0 |
| **Static linking** | LGPLv3 requires relink freedom | EPL is commercial-friendly, non-viral |
| **Maintainer** | Qt Group | Eclipse Foundation |
| **TLS** | Qt's SSL stack (consistent with QNetwork) | Direct OpenSSL integration |
| **Threading** | Qt event loop + QThread | paho-owned dispatcher thread, no Qt loop needed |
| **Non-GUI** | Still needs QCoreApplication | Runs standalone |
| **API style** | Native Qt signals/slots | Callbacks (already wrapped) |
| **vcpkg requirement** | None | `paho-mqttpp3` + `paho-mqtt` |

**Rules of thumb**:
- Commercial closed-source industrial deployment → **paho** (clean license)
- Internal Qt-only tooling / prototype → **Qt6::Mqtt** (smaller, tighter integration)
- LGPLv3 audit risk is high → **paho**

The two backends are gated by `CORE_BUILD_MQTT_QT` and
`CORE_BUILD_MQTT_PAHO` in `core/CMakeLists.txt` (both default ON); each
gracefully degrades to a stub when the library isn't found, so the
project still compiles either way.

### OPC UA node mapping

`OpcUaClientTransport` maps Modbus-style `ReadRequest` (`{table, addr, count}`)
to OPC UA nodes via `node_id_template`:

```toml
[[transport]]
id   = "opc1"
kind = "opc_ua_client"
endpoint_url     = "opc.tcp://192.168.10.5:4840"
node_id_template = "ns=2;s=Var_%1"   # %1 substituted with addr
backend          = "open62541"        # Qt OpcUa backend
```

- Default backend = `open62541` (the open-source MIT backend bundled with
  Qt OpcUa)
- For production OPC UA usage, an OPC-UA-specific PollRange/SinkWindow
  variant will land in Phase 4; the current register→node mapping is the
  simplest path to fit OPC UA into Core's existing abstractions

### Pending

| Protocol | Class | Library |
|----------|-------|---------|
| Siemens S7 | `S7ClientTransport` | [snap7](http://snap7.sourceforge.net/) v1.4.2 (LGPL) |

The S7 stub returns `"snap7 library not yet vendored"` from `connect()`.
If your field deployment has Siemens S7 hardware, build snap7 and tell us
where it lives.

### Other protocols on the radar

- HTTP / REST: built into Qt6::Network (`QNetworkAccessManager`), no
  external library required
- Raw TCP / UDP: built into Qt6::Network (`QTcpSocket` / `QUdpSocket`)
- gRPC / protobuf: already in the project's `vcpkg.json`, reusable

---

## 7. Examples

| Directory | Demonstrates |
|-----------|-------------|
| `examples/minimal_modbus/`      | The bare minimum — 3 datapoints, auto-reconnect |
| `examples/operator_box_to_plc/` | Operator-box ↔ PLC routing bridge |
| `examples/stats_dashboard/`     | Real-time scheduler statistics |

Each example carries a runnable `main.cpp`, a TOML config, and its own
README.

---

## 8. Migration from Legacy Core

The new Core lives at `core/` and is completely independent of
`src/Core/`. They can coexist while applications migrate one by one.

| Legacy Core | New Core |
|-------------|----------|
| `Core::loadPlugins`           | `ICore::loadConfig + plugins().loadAll` |
| `JMJAttributes` Q_PROPERTY    | `Datapoint` Q_PROPERTY + DatapointQmlBridge |
| `JMJDatabase` Q_INVOKABLE     | Subscribe to `DpChanged` + persistTag routing (Phase 3+) |
| INI config                    | TOML (schema in §4) |
| Lua decoding                  | builtin scale/mask/wordOrder + EnumU16Codec (LuaCodec Phase 3+) |
| `ModbusPoll` coroutine        | `PollRange` + `RequestScheduler` |

Step-by-step migration plan: see spec §Phase 4.

---

## Feedback

- Bugs / feature requests: file a repo issue
- Protocol integration requests: just say "I want to integrate X". I'll
  evaluate the library choice and tell you exactly what to compile.
