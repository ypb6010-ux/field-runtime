# Core 使用手册（简体中文）

> 工业 Modbus / OPC UA / MQTT / S7 多协议运行时，带类型化数据点、声明式路由、半双工感知调度。

## 目录

1. [概述](#1-概述)
2. [快速开始](#2-快速开始)
3. [架构](#3-架构)
4. [TOML 配置参考](#4-toml-配置参考)
5. [调度器选型](#5-调度器选型)
6. [多协议 Transport](#6-多协议-transport)
7. [示例](#7-示例)
8. [从旧 Core 迁移](#8-从旧-core-迁移)

---

## 1. 概述

Core 是为矿用/工业 SCADA 上位机设计的运行时库，把"PLC ↔ 操作箱 ↔ 上位机"
三层通信抽象成几个明确的概念：

- **Transport** — 一条物理连接（Modbus TCP/RTU、OPC UA、MQTT、S7 等）
- **Scheduler** — 一条 Transport 内部的请求调度器（Serial / Credit / Priority）
- **Datapoint** — 一个具名的逻辑信号，自带类型 + 编解码管道
- **Module** — 周期或事件驱动的运行单元（PollRange、SinkWindow、Heartbeat、
  Command、AckWatch）
- **EventBus** — 类型化发布/订阅，跨线程安全
- **ConfigLoader** — TOML 解析 + 启动期 fail-fast 校验

整套库是 C++23 + Qt 6.8 实现，依赖 `async_simple`（协程）、`tomlplusplus`
（解析）、`Catch2`（测试）。

---

## 2. 快速开始

### 2.1 构建

需要 Qt 6.8、CMake 3.21、MSVC 19.36+ / GCC 13 / Clang 16：

```powershell
cmake -S core -B core/build -G Ninja `
    -DCMAKE_BUILD_TYPE=Debug `
    -DQt6_DIR=$env:QT6_DIR
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

### 2.2 三个最小示例

```bash
# 1) 最小：读 3 个数据点
example_minimal_modbus  examples/minimal_modbus/minimal.toml

# 2) 操作箱 → PLC 桥接
example_operator_box_to_plc  examples/operator_box_to_plc/bridge.toml

# 3) 调度统计面板
example_stats_dashboard  examples/stats_dashboard/dashboard.toml
```

详见 `core/examples/` 下各 README.md。

### 2.3 在自己的 App 里集成

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

// 订阅数据点变化
auto sub = core->bus().subscribe<core::bus::DpChanged>(
    [](core::bus::DpChanged const& e) {
        qInfo() << e.id << "=" << e.value;
    });

core->start();
return app.exec();
```

---

## 3. 架构

```
                        ┌──────────────────────────────┐
                        │           ICore               │
                        │  (facade — start/stop/load)   │
                        └─────┬───────────────┬─────────┘
                              │               │
              ┌───────────────▼──┐         ┌──▼────────────┐
              │  ModuleRegistry  │         │  EventBus     │
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

**关键数据流（典型 PLC 读取场景）**：

```
PLC ──Modbus──► Transport.read
            ──► RequestScheduler.submit  (排队 / 限流 / 熔断)
            ──► PollRange.pollOnce
            ──► Codec.decode (byte order + scale/offset/mask)
            ──► Datapoint.setValue
            ──► emit valueChanged
            ──► EventBus.publish(DpChanged)
            ──► QML / 插件 / 数据库订阅者收到
```

**操作箱→PLC 场景**（路由由 TOML 的 `[[route]]` 决定）：

```
操作箱 ──Modbus写──► ServerTransport.dataWritten
                ──► EventBus.publish(ServerWriteEvent)
                ──► Core 路由查找 from→to
                ──► SinkWindow.stageRegister
                ──► QTimer 周期 onTick → flush
                ──► PLC client.writeBatch
                ──► PLC
```

---

## 4. TOML 配置参考

完整 schema 见 [`doc/design/Core-Greenfield-Spec.md` §四](../../../doc/design/Core-Greenfield-Spec.md#4-配置层-toml-schema-全集)。
这里给出最常用字段的速查表。

### 4.1 `[[transport]]`

```toml
[[transport]]
id   = "plc"                 # 全局唯一
kind = "modbus_tcp_client"   # 或 modbus_tcp_server / modbus_rtu / opc_ua_client / mqtt_client / s7_client

# Modbus TCP client / server / S7
host = "192.168.0.10"
port = 502
slave_id = 1
listen_address = "0.0.0.0"   # server only
listen_port    = 502         # server only

# Modbus RTU
port_name = "COM3"           # 或 /dev/ttyUSB0
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

# 通用
reconnect_interval_ms = 15000     # 0 关闭自动重连
connect_timeout_ms    = 3000
request_timeout_ms    = 1000

[transport.scheduler]
kind                       = "serial"   # serial / credit / priority
max_inflight               = 1          # credit / priority 用
inter_request_gap_ms       = 5          # 半双工建议 5~10
max_queue_depth            = 256
starvation_guard_ms        = 5000       # priority 用
fifo_within_lane           = true
circuit_breaker_threshold  = 10
circuit_breaker_open_ms    = 5000

# 服务端模式可声明监听范围
[[transport.listen_ranges]]
table = "HR"
range = [0, 64]
```

### 4.2 `[[poll_range]]` / `[[sink_window]]` / `[[heartbeat]]` / `[[command]]` / `[[ack_watch]]`

```toml
# 周期读
[[poll_range]]
module_id = "poll.plc.hr"
transport = "plc"
table     = "HR"
range     = [0, 16]        # [起始地址, 寄存器数]
period_ms = 200
priority  = "Normal"       # Low / Normal / High / Critical

# 批量写窗口（Modbus FC16 上限 123 寄存器）
[[sink_window]]
module_id = "sw.plc"
transport = "plc"
table     = "HR"
range     = [100, 4]       # [起始地址, 大小]
priority  = "High"
initial   = [0, 0, 0, 0]
[sink_window.flush]
debounce_ms  = 20          # 最后一次 stage 后等多久 flush
keepalive_ms = 5000        # 即使没变化，超过此间隔也 flush 一次
coalesce     = true        # 多次相邻 stage 合并

# 心跳
[[heartbeat]]
module_id = "hb.plc"
transport = "plc"
table     = "HR"
address   = 999
value     = 1              # 或 values = [1, 2, 3]
period_ms = 1000

# 一次性命令
[[command]]
module_id = "cmd.start"
transport = "plc"
priority  = "High"
interruptable = false
[[command.writes]]
table = "HR"
address = 200
value = 1

# 反馈等待
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

### 4.4 启动期校验

加载阶段强制 fail-fast 校验，详见 spec §4.3。常用规则：

- id 跨节全局唯一（transport / module_id / datapoint / codec）
- 所有 transport/datapoint/codec/sink_window 引用必须命中已声明项
- 32 位类型必须声明 `wordOrder`
- Bool 类型必须声明 `bit`
- EnumU16 类型必须声明 `codec`
- `sink_window.range[1] ≤ 123`
- `datapoint.sink.addr` 必须落入引用窗口 `[start, start+size)`
- `mask` 不超过 `type` 位宽
- `policy=UntilAck` 必须有 `[datapoint.ack]` 块

错误格式带字段名和行号：

```
[Config Error] datapoint[7] "belt2.cmd.start"
  field: source.wordOrder
  message: type=F32 requires wordOrder (ABCD/CDAB/BADC/DCBA), got null
  line: 142
```

---

## 5. 调度器选型

| 调度器 | 何时用 | 关键配置 |
|--------|--------|---------|
| **Serial** | 半双工设备（RS-485、Modbus RTU、共用 TCP 网关） | `inter_request_gap_ms=5~10` |
| **Credit** | 全双工设备（OPC UA、MQTT、原生 TCP） | `max_inflight=4~16` |
| **Priority** | 紧急停车场景（需高优 50ms 内抢占） | `starvation_guard_ms=5000`、`tag.interruptable=true` |

行为总览：

- 所有调度器都是优先级 4 级（Critical > High > Normal > Low），lane 内
  按 `fifo_within_lane` 选 FIFO 或 round-robin
- Credit 允许同时有 `max_inflight` 个请求飞行
- Priority 在 `starvation_guard_ms` 超时后跨优先级提升被饿死的 lane
- 任意调度器都会在错误连续 `circuit_breaker_threshold` 次后熔断，
  `circuit_breaker_open_ms` 后进入 HalfOpen

订阅 `bus::SchedulerStatsEvent` 可以拿到每个 transport 的 queue / inflight /
p50 / p99 / 熔断状态实时快照。

---

## 6. 多协议 Transport

### 已完整实现

| 协议 | 类名 | 依赖 |
|------|------|------|
| Modbus TCP client | `ModbusTcpClientTransport` | Qt6::SerialBus |
| Modbus TCP server | `ModbusTcpServerTransport` | Qt6::SerialBus |
| Modbus RTU       | `ModbusRtuTransport`       | Qt6::SerialBus + Qt6::SerialPort |

### 骨架已就位，等编译外部库

`connect()` 在外部库未提供时返回 `"library not vendored"`，但 TOML schema、
Config 字段、生命周期接口已稳定，库编完后只需替换 `Impl`。

| 协议 | 类名 | 库 | 推荐版本 |
|------|------|------|---------|
| OPC UA   | `OpcUaClientTransport` | [open62541](https://github.com/open62541/open62541) | v1.4.x（MIT） |
| MQTT     | `MqttClientTransport`  | [paho.mqtt.cpp](https://github.com/eclipse/paho.mqtt.cpp) | v1.4.x（EPL） |
| Siemens S7 | `S7ClientTransport`  | [snap7](http://snap7.sourceforge.net/) | v1.4.2（LGPL） |

请把编译好的库连同 `find_package` 信息发我，我接入实现。

### 其他可选协议（待评估）

- HTTP / REST：Qt6::Network 内置 `QNetworkAccessManager`，无需外部库
- 原生 TCP / UDP：Qt6::Network 内置 `QTcpSocket` / `QUdpSocket`
- gRPC / protobuf：项目 `vcpkg.json` 已含，可复用

---

## 7. 示例

| 目录 | 演示 |
|------|------|
| `examples/minimal_modbus/`        | 最小示例：3 个数据点 + 自动重连 |
| `examples/operator_box_to_plc/`   | 操作箱 ↔ PLC 路由桥接 |
| `examples/stats_dashboard/`       | 实时调度统计 |

每个示例下都有可运行的 `main.cpp`、TOML 配置和单独的 README。

---

## 8. 从旧 Core 迁移

新 Core 与 `src/Core/` 完全独立，可并行运行直到 App 全部迁完。

| 旧 Core | 新 Core |
|---------|---------|
| `Core::loadPlugins`           | `ICore::loadConfig + plugins().loadAll` |
| `JMJAttributes` Q_PROPERTY    | `Datapoint` Q_PROPERTY + DatapointQmlBridge |
| `JMJDatabase` Q_INVOKABLE     | 订阅 `DpChanged` + persistTag 路由（Phase 3+） |
| INI 配置                       | TOML（schema 参见 §4） |
| Lua 解码                       | builtin scale/mask/wordOrder + EnumU16Codec（LuaCodec Phase 3+） |
| `ModbusPoll` 协程             | `PollRange` + `RequestScheduler` |

详细迁移步骤见 spec §Phase 4 章节。

---

## 反馈

- Bug / 提案：在仓库提 Issue
- 协议接入需求：直接说"我要接 XX 协议"，我会评估库选型并告诉你需要编译什么
