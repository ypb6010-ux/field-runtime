# Core —— 工业 Modbus/多协议 运行时

> 一个以**声明式配置(TOML)**驱动、**事件总线**解耦、**全异步非阻塞 I/O**的设备采集与控制内核。
> 把"协议寄存器"抽象成"带类型的数据点(datapoint)",把"采集/下发/心跳/命令"抽象成"模块",
> 业务侧只面对 datapoint 和事件,不碰寄存器细节。

---

## 目录

1. [它是什么 / 设计原则](#1-它是什么--设计原则)
2. [子系统与引用关系](#2-子系统与引用关系)
3. [架构图](#3-架构图)
4. [数据流程图(上行采集 / 下行控制)](#4-数据流程图)
5. [消息总线 EventBus](#5-消息总线-eventbus)
6. [核心概念](#6-核心概念)
7. [TOML 配置详解(逐节示例)](#7-toml-配置详解)
8. [Lua codec 详解](#8-lua-codec-详解)
9. [扩展点](#9-扩展点)
10. [构建开关](#10-构建开关)
11. [最小可运行示例](#11-最小可运行示例)

---

## 1. 它是什么 / 设计原则

- **声明式优先**:绝大多数接入(连哪台 PLC、读哪些寄存器、怎么解码、怎么落库、怎么下发)
  都写在一个 `*.toml` 里,改配置不改代码。
- **datapoint 为中心**:`寄存器 → Codec 解码 → Datapoint(带类型/状态/时间戳的 QObject)`。
  QML 直接绑 `Datapoint.value`,业务逻辑订阅 `DpChanged` 事件。
- **全异步、不阻塞 GUI**:所有 transport 的 `readAsync/writeAsync` 非阻塞;采集 tick 跑在
  GUI 事件循环里(不再每 transport 一个工作线程),回包在各自 client 线程上交付。
- **事件总线解耦**:子系统之间不直接调用,通过 `EventBus` 发布/订阅类型化事件。
- **可扩展**:DLL 插件、自定义 Codec(含 Lua 脚本)、自定义日志 Sink、自定义 Transport。

---

## 2. 子系统与引用关系

公共头文件都在 `include/core/<子系统>/`。各子系统的**依赖方向**(谁引用谁):

| 子系统 | 目录 | 职责 | 依赖 |
|---|---|---|---|
| **ICore** | `ICore.h` | 门面:加载配置、持有并装配所有子系统、`start/stop` | 全部 |
| **config** | `config/` | TOML 解析(`ConfigLoader`)→ 结构体(`ConfigSchema`)+ 校验 | — |
| **transport** | `transport/` | 协议 I/O(Modbus TCP/RTU、OPC UA、MQTT、S7);`readAsync/writeAsync` | sched, bus |
| **sched** | `sched/` | 请求限流/优先级/熔断/半双工 gap;`submitAsync` 事件驱动 | — |
| **codec** | `codec/` | 寄存器 ↔ 值 编解码(标量/枚举/Lua) | dp(PortRef) |
| **dp** | `dp/` | `Datapoint`(QObject)+ `DatapointRegistry`;`PortRef` 寻址 | codec |
| **module** | `module/` | 采集/下发/心跳/命令:`PollRange`/`SinkWindow`/`Heartbeat`/`Command`/`AckWatch` | transport, sched, dp, bus |
| **bus** | `bus/` | `EventBus`(类型化发布/订阅)+ 标准事件 | — |
| **log** | `log/` | `Logger`(异步分发)+ `ILogSink` + `LogBridge`(QML) | — |
| **plugin** | `plugin/` | DLL 插件加载 + 端口绑定(`InPort/OutPort` ↔ datapoint) | dp, bus |
| **qml** | `qml/` | `DatapointQmlBridge` / `LogBridge`(注入 QML context) | dp, log |
| **persistence** | `persistence/`(独立模块) | QxOrm/Postgres 落库(telemetry/operation_log/system_log) | bus, log |
| **coro** | `coro/` | `Lazy/Task`(re-export async-simple) | — |

**一句话引用链**:
`ConfigLoader` 读 TOML → `ICore` 据此建 `Transport`(各自挂一个 `Scheduler`)、`Datapoint`(各自绑一个
`Codec`)、`Module`;`Module` 通过 `Scheduler` 驱动 `Transport` 读写,把结果写进 `Datapoint` 并经
`EventBus` 广播;`Plugin`/`Persistence`/`QML 桥` 都只订阅 `EventBus` 或读 `Datapoint`,互不直接耦合。

---

## 3. 架构图

```
                            ┌───────────────────────┐
                            │         ICore         │  门面 · 装配 · 生命周期
                            │  loadConfig / start    │
                            └───────────┬───────────┘
        ┌───────────┬───────────┬───────┼────────┬────────────┬───────────┐
        ▼           ▼           ▼       ▼        ▼            ▼           ▼
   Transport    Scheduler   Datapoint  Codec   Module      EventBus    Logger
   协议 I/O      限流/熔断    Registry  Registry Registry    消息总线     异步日志
        │  ▲        ▲          ▲        ▲        │             ▲           │
        │  └─每个 transport 持有一个─┘            │             │           ├─ ConsoleSink
        │                       │        │        │             │           ├─ RollingFileSink
        │      Datapoint ──绑定──┴────────┘        │             │           └─ 自定义/DbLogSink
        │           ▲                             │             │
        └─readAsync─┘   Module(PollRange/Sink…) ──提交→Scheduler─┘──publish──┤
                                                                            │
   ┌────────────────────────────────────────────────────────────────┐     │
   │ 订阅方(互不耦合,只认 EventBus / Datapoint):                    │◄────┘
   │  • Plugin(DLL) ── PortRegistry ── InPort/OutPort ↔ Datapoint    │
   │  • Persistence ── 订阅 DpChanged → telemetry;Logger sink → 日志表 │
   │  • QML ── DatapointQmlBridge / LogBridge(context property)       │
   └────────────────────────────────────────────────────────────────┘
```

---

## 4. 数据流程图

### 上行(采集)

```
PLC ──Modbus──► Transport.readAsync ──► Scheduler(选请求·限流·熔断·gap)
   ──► PollRange(每 period_ms 一次 tick,m_inFlight 合并防堆积)
   ──► Codec.decode(raw 寄存器 → 带类型的值;字序/掩码/scale/offset 或 Lua/枚举)
   ──► Datapoint.setValue(value, state, ts)
   ──► EventBus.publish(DpChanged{id, value, ts})
          ├──► QML 绑定刷新(Datapoint.value 是 Q_PROPERTY,NOTIFY 自动更新)
          ├──► Plugin 的 InPort<T>
          └──► Persistence(带 persist 标签的点 → telemetry 表)
```

### 下行(控制)

```
来源 A:操作箱(Modbus Server)写寄存器
   Transport(server) ──► EventBus.publish(ServerWriteEvent{transportId,table,addr,values})
来源 B:QML/业务调用 Datapoint.write() 或 Plugin 的 OutPort<T>.send()

   ──► (Route 路由 / 适配器 / 插件逻辑:可做拆位、换算等)
   ──► SinkWindow.stageRegister(absAddr, value, mask)   把值暂存进窗口快照
   ──► (debounce 去抖 / keepalive 周期重写 / coalesce 合并)
   ──► Scheduler ──► Transport.writeAsync ──► PLC
```

---

## 5. 消息总线 EventBus

`include/core/bus/EventBus.h` —— 类型化、线程安全的发布/订阅。

```cpp
template <class T> Subscription subscribe(std::function<void(T const&)> handler);
template <class T> void         publish(T const& event);
```

- `publish<T>` 可在任意线程调用;**处理器同步运行在发布者线程**(故 handler 要短、要线程自洽)。
- `subscribe<T>` 返回一个 RAII 的 `Subscription`,**必须持有它**,析构即退订。

### 标准事件(`bus/BusEvents.h`)

| 事件 | 字段 | 谁发 | 谁订 |
|---|---|---|---|
| `DpChanged` | `id, value, timestamp` | datapoint 更新时 | QML 桥、插件、持久化 |
| `TransportEvent` | `transportId, kind, payload` | transport 连接/断开/熔断 | 状态显示、日志 |
| `ServerWriteEvent` | `transportId, table, startAddress, values` | Modbus Server 收到操作箱写 | 路由 / 控制适配器 |
| `SchedulerStatsEvent` | `transportId, stats` | 每 ~1s 的统计泵 | 仪表盘(队列深度/p50/p99/熔断) |
| `CoreReady` / `CoreStopping` | — | 生命周期 | 需要在 core 就绪/停机时动作的订阅者 |

`TransportEventKind`:`Connected/Disconnected/CircuitOpened/CircuitClosed/CircuitHalfOpen/ReadCompleted/WriteCompleted`。

---

## 6. 核心概念

### Transport(`transport/Transport.h`)
一条协议连接。统一接口:`connect/disconnect/state`、同步 `read/writeBatch`、**异步 `readAsync/writeAsync`**(非阻塞)、`scheduler()`。实现:Modbus TCP Client/Server、Modbus RTU、OPC UA Client、MQTT(Qt/paho)、S7(桩)。每条 transport 自带一个 `Scheduler`。

### Scheduler(`sched/RequestScheduler.h`)
请求网关。`submitAsync(tag, work)` 事件驱动:work 发起非阻塞 I/O,完成回调里泵下一个。
- `SerialScheduler` —— 串行(半双工,如 RTU;`inter_request_gap_ms` 用 client 线程定时器实现)
- `CreditScheduler` —— 并发在途(全双工,如 OPC UA/MQTT)
- `PriorityScheduler` —— 优先级 + 防饿;高优先级可抢占可中断的低优先级请求
含:熔断器、队列上限、`stopAsync()`(teardown 时停泵)。

### Datapoint(`dp/Datapoint.h`)
一个带类型/状态/时间戳的逻辑信号,**本身是 QObject**,QML 可直接绑:
```
Q_PROPERTY value / valid / ts / state
Q_INVOKABLE void write(QVariant)   // 经 sink 写回 PLC
source() / sink()  →  PortRef(transport, table, addr, bit, wordOrder, shift, mask, scale, offset, codec, window)
```
`DpState`:`Ok / Stale / Error / Missing`。

### Codec(`codec/Codec.h`)
`decode(raw 寄存器) → 值` / `encode(值) → 寄存器`。三种:
- `BuiltinScalarCodec` —— 全 11 种 ScalarType × 4 种 WordOrder + shift/mask + `scale*x+offset`
- `EnumU16Codec` —— u16 → 枚举文字(查表)
- `LuaCodec` —— Lua 脚本(见 [§8](#8-lua-codec-详解))

### Module(`module/FunctionalModule.h`)
有 `tickPeriodMs()` + `driveTick()` 的周期任务,由 `ModuleRegistry` 在 **GUI 事件循环**上挂表驱动:
- `PollRange` —— 周期读一段寄存器 → 解码 → 更新 datapoint
- `SinkWindow` —— 把多个写暂存进一段连续寄存器的快照,按去抖/keepAlive/reconnect 批量 flush
- `Heartbeat` —— 周期写心跳值(可自增/时间戳)
- `Command` —— 一组寄存器写(触发式)
- `AckWatch` —— 等待某 datapoint 达到期望值(超时)

---

## 7. TOML 配置详解

一个配置文件由若干"节"组成。下面逐节说明 + 示例。完整最小例见 [§11](#11-最小可运行示例)。

### `[meta]`
```toml
[meta]
project   = "demo"
version   = "0.1"
log_level = "info"     # trace|debug|info|warn|error|critical;空=默认
```

### `[[transport]]` —— 协议连接
```toml
[[transport]]
id    = "plc1"
kind  = "modbus_tcp_client"   # modbus_tcp_client/modbus_tcp_server/modbus_rtu/
                              # opc_ua_client/mqtt_client/mqtt_paho_client/s7_client
host  = "127.0.0.1"
port  = 502
slave_id = 1
connect_timeout_ms    = 3000
request_timeout_ms    = 1000
reconnect_interval_ms = 15000

[transport.scheduler]
kind                 = "serial"   # serial / credit / priority
inter_request_gap_ms = 5          # 半双工最小间隔(RTU 关键)
max_inflight         = 1          # credit 模式并发在途数
max_queue_depth      = 256
starvation_guard_ms  = 5000       # priority 模式防饿
circuit_breaker_threshold = 10    # 连续失败多少次开熔断
circuit_breaker_open_ms   = 5000
```
其它 kind 的专属键:
- **server**:`listen_address` / `listen_port` / `max_clients` / `[[transport.listen_ranges]]`(`table`+`range=[start,size]`)
- **rtu**:`port_name="COM3"` / `baud_rate` / `data_bits` / `stop_bits` / `parity="none|even|odd"`
- **opc_ua**:`endpoint_url` / `security_policy` / `username` / `password` / `node_id_template="ns=2;s=Var_%1"`
- **mqtt**:`broker_uri="tcp://host:1883"` / `client_id` / `topic_prefix` / `topic_template="reg/%1"` / `qos` / `clean_session`
- **s7**:`rack` / `slot`

### `[[codec]]` —— 自定义编解码(内置标量无需声明)
```toml
[[codec]]
id   = "run_state"
kind = "enum_u16"
map  = { "0" = "停机", "1" = "运行", "2" = "故障" }

[[codec]]
id     = "bcd_time"
kind   = "lua"
script = "codec/bcd_datetime.lua"   # 相对 config 文件目录
arg    = ""                          # 透传给脚本 ctx.arg(可选,见 §8)
```

### `[[poll_range]]` —— 周期采集
```toml
[[poll_range]]
module_id = "poll.plc1.hr"
transport = "plc1"
table     = "HR"          # HR/HoldingRegisters · IR/InputRegisters · Coil · DI
range     = [0, 8]        # [起始地址, 寄存器个数]
period_ms = 500
priority  = "Normal"      # Low/Normal/High/Critical
```

### `[[datapoint]]` —— 数据点(核心)
```toml
[[datapoint]]
id      = "temperature"
kind    = "Status"        # Status(只读) / Command(只写) / Bidirectional
type    = "S16"           # Bool/U16/S16/U32/S32/F32/U64/S64/F64/EnumU16/String
persist = "plc1.temperature"   # 设了就落 telemetry 表;不设不落库
# 读端:从哪个寄存器解码
source  = { port = "plc1", table = "HR", addr = 0, scale = 0.1, codec = "" }
# 写端(可选):写回到哪(直连寄存器,或某个 sink_window)
# sink  = { port = "plc1", table = "HR", addr = 100 }      # 或 window = "win.cmd"
```
`source`/`sink` 即 `PortRef`,可带:`addr`、`bit`(取某位)、`wordOrder`(ABCD/CDAB/BADC/DCBA)、
`shift`/`mask`(位域)、`scale`/`offset`(线性)、`codec`(指定自定义 codec id)、`window`(写进某 sink_window)。

### `[[sink_window]]` —— 批量写窗口(控制下发)
```toml
[[sink_window]]
module_id = "win.cmd"
transport = "plc1"
table     = "HR"
range     = [100, 4]      # [起始, 大小]
[sink_window.flush]
debounce_ms  = 20         # 暂存后多久 flush
keepalive_ms = 100        # >0 则周期重写(在线保活;断线重连自动 forceFlush)
coalesce     = true       # 合并写
max_retries  = 2
```

### 其余节(简）
```toml
[[heartbeat]]  module_id="hb" transport="plc1" table="HR" address=200 value=1 period_ms=1000  # 或 values=[...] / incrementer="u16_inc|timestamp"
[[command]]    module_id="reset" transport="plc1" trigger="..." writes=[{table="HR",address=10,value=1}]
[[ack_watch]]  module_id="ackOpen" dp="breaker" expected=1 timeout_ms=3000
[[route]]      name="opbox→plc" from="opbox.cmd" to="plc.cmd" policy="ContinuousMirror"
[[plugin]]     name="MyLogic" dll="Plugins/MyLogic.dll" config=""    # DLL 插件,见 §9
```

> `[[route]]` 是**逐 datapoint**的单点映射;要把操作箱 server 与 PLC client 做**整段双向
> 中继**(旧 `ModbusServer` 的活),用下面的 `[[bridge]]`。

### `[[bridge]]` 整段桥接(操作箱 ↔ PLC)

把操作箱连接的 `modbus_tcp_server` 与 PLC 侧 `modbus_tcp_client` 整段双向桥接,替代旧
`ModbusServer` 的中继:

```toml
[[bridge]]
server      = "main"      # 操作箱连的 server transport id
plc         = "default"   # PLC client transport id
offset      = 0           # server 地址 = plc 地址 + offset
write_start = 0           # 转发区:server 写 [write_start, write_start+write_count) → PLC
write_count = 50
mirror_start = 50         # 镜像区:PLC 读 [mirror_start, mirror_start+mirror_count) → server 寄存器
mirror_count = 300
mirror_period_ms = 100    # 镜像刷新周期(默认 100)
```

- **转发**(写区):操作箱写 server 触发 `ServerWriteEvent` → 命中写区的子段 `stageRegister`
  到 PLC 侧一个**自动创建的 SinkWindow**(`bridge.fwd.<server>`,High 优先级,带重试/coalesce)
  → 由 TickDriver 刷到 PLC。地址按 `plc = server - offset` 映射。
- **镜像**(读区):每 `mirror_period_ms` 把 PLC 读区的 datapoint 值整段 `writeBatch` 回 server
  transport **自己的寄存器表**,操作箱即可读到 PLC 状态。镜像区必须有来自 `plc` 的 HR datapoint
  覆盖(否则镜像恒为 0,加载时校验会报错)。
- 写区/读区**不重叠**:写区保留操作箱写入(不被镜像覆盖),读区回显 PLC。

> 校验:`ConfigLoader::validate` 会查"引用是否命中已声明项""id 唯一""sink.addr 是否落在所引用
> 的 sink_window 范围内""bridge.server/plc transport 是否存在""bridge 镜像区是否有 PLC datapoint"
> 等,出错会在加载时把 section/field/行号一并报出。

---

## 8. Lua codec 详解

`LuaCodec`(`codec/LuaCodec.h`)让你用一段 Lua 脚本表达"内置流水线写不了的"编解码(BCD、位域复合、
非线性查表、厂商私有帧、按现场可改的故障表…)。需开 `CORE_BUILD_LUA=ON`。

### 脚本契约
脚本 `return` 一个带 `decode`/`encode` 的表:
```lua
return {
  -- raw:1-indexed 的寄存器数组(整数 0..65535)
  -- ctx:{ address, count, scale, offset, arg }
  decode = function(raw, ctx)
    return value        -- 数字 / 字符串 / 布尔
  end,
  encode = function(value, ctx)
    return { r1, r2 }   -- 寄存器数组
  end,
}
```
- 寄存器个数由 datapoint 的 `type` 决定(`U16`→1、`U32`→2…),`raw` 就是这几个寄存器。
- `ctx.arg` = `[[codec]]` 里的 `arg` 字段(透传的判别符),**一个脚本可服务多个变体**:
  ```toml
  [[codec]]
  id="fault_high" kind="lua" script="codec/fault.lua" arg="high"
  [[codec]]
  id="fault_low"  kind="lua" script="codec/fault.lua" arg="low"
  ```
  ```lua
  local tbl = ({ high = {...}, low = {...} })[ctx.arg] or {}
  ```

### 能力边界(故意划低,图绝对安全可预测)
| 维度 | 边界 | 由什么制定 |
|---|---|---|
| 数据范围 | 只看得到**本数据点的寄存器** + ctx | `Codec` 接口签名 |
| 副作用 | **纯函数**,不能写别的寄存器/发事件/写日志/碰 DB | 不给任何 core 句柄 |
| 沙箱 | 只开 `base/math/string/table`,**无 io/os/package/coroutine/debug**;并显式移除 base 里的 `dofile/loadfile/load/loadstring`(否则它们能读/执行任意文件) | `LuaCodec.cpp` 的 `open_libraries` + nil 掉加载器 |
| 时间 | **同步内联**在数据路径上跑、互斥串行,必须立刻返回(不能 sleep/长循环) | 调用模型 |

> ⚠️ 已知缺口:**未做 CPU/内存配额**。沙箱防"越权"(碰 OS),但防不住"失控"(死循环会卡住该
> transport 的数据路径)。接不可信脚本前需补 `lua_sethook` 指令上限 + 限制内存的 allocator。

一句话:**它最多是个"纯的、沙箱的、同步的、单点寄存器↔值变换函数"**——拿不到系统,所以闯不了祸。

---

## 9. 扩展点

| 想扩展 | 怎么做 |
|---|---|
| **业务逻辑(DLL 插件)** | 实现 `plugin/Plugin.h` 的 `Plugin`,在 `registerPorts(PortRegistry&)` 里把 `InPort<T>`(收 datapoint 变化)/`OutPort<T>`(写 datapoint)绑到点上;用 `CORE_PLUGIN_ENTRY(MyPlugin)` 导出 `corePluginCreate`;TOML `[[plugin]] dll="..."` 加载。 |
| **自定义编解码** | 简单/可现场改 → **Lua codec**(§8);需要 C++ 性能/复杂逻辑 → 实现 `codec/Codec.h` 的 `Codec` 注册进 `CodecRegistry`。 |
| **自定义日志去向** | 实现 `log/ILogSink.h` 的 `ILogSink`(`write(LogRecord)`/`write(OperationRecord)`),`logger().addSink(...)`。`Persistence` 的 `DbLogSink`、UI 实时日志都是这么接的。 |
| **自定义协议** | 实现 `transport/Transport.h`,提供非阻塞 `readAsync/writeAsync`(否则会阻塞 GUI 线程 tick)。 |
| **QML 接入** | `ICore::create(qmlContext)` 自动注入 `log` 桥;datapoint 本身是 QObject,作为 context property 或经 `DatapointQmlBridge` 暴露后直接绑 `value`。 |

> 插件模型(InPort/OutPort)只给 datapoint 端口,**不给原始 EventBus / 核心句柄**——刻意保持窄,
> 业务逻辑("操作箱拆位""跨点告警/联锁")若需要更多上下文,放 app 层适配器,或显式扩展插件契约。

---

## 10. 构建开关

`core/CMakeLists.txt`:

| 选项 | 默认 | 说明 |
|---|---|---|
| `CORE_BUILD_TESTS` | ON | Catch2 单测 |
| `CORE_BUILD_EXAMPLES` | ON | 示例(`examples/`) |
| `CORE_BUILD_QML` | ON | QML 集成(LogBridge/DatapointQmlBridge) |
| `CORE_BUILD_LUA` | OFF | Lua codec(需 sol2 + lua5.4) |
| `CORE_BUILD_OPCUA` | ON | OPC UA Transport(Qt6::OpcUa) |
| `CORE_BUILD_MQTT_QT` | ON | MQTT(Qt6::Mqtt) |
| `CORE_BUILD_MQTT_PAHO` | ON | MQTT(paho.mqtt.cpp;找不到自动关) |
| `CORE_BUILD_PERSISTENCE` | OFF | 持久化模块(QxOrm/Postgres) |

依赖:Qt6(Core/Network/SerialBus/SerialPort/Sql,+ 可选 OpcUa/Mqtt/Qml)、async-simple、tomlplusplus;
可选 sol2+lua、QxOrm、paho。MSVC 强制 `/EHsc`(`ConfigLoader` 用异常包 `toml::parse`)。

---

## 11. 最小可运行示例

`demo.toml`:
```toml
[meta]
project = "demo"
log_level = "info"

[[transport]]
id = "plc1"
kind = "modbus_tcp_client"
host = "127.0.0.1"
port = 502
[transport.scheduler]
kind = "serial"
inter_request_gap_ms = 5

[[poll_range]]
module_id = "poll.plc1"
transport = "plc1"
table = "HR"
range = [0, 4]
period_ms = 500

[[codec]]
id = "run_state"
kind = "enum_u16"
map = { "0" = "停机", "1" = "运行", "2" = "故障" }

[[datapoint]]
id = "temperature"
type = "S16"
kind = "Status"
persist = "plc1.temperature"
source = { port = "plc1", table = "HR", addr = 0, scale = 0.1 }

[[datapoint]]
id = "state"
type = "EnumU16"
kind = "Status"
source = { port = "plc1", table = "HR", addr = 1, codec = "run_state" }
```

代码(C++):
```cpp
#include "core/ICore.h"
auto core = core::ICore::create(engine.rootContext());   // 注入 log 桥
if (auto r = core->loadConfig("demo.toml"); !r) { /* 打印 r.error() */ }
core->start();
// QML 里:绑 datapoint 的 value;订阅 core->bus() 的 DpChanged;等等。
```

更多可跑示例见 `core/examples/`(`example_qml_dashboard` 是带 UI 的综合示例),
启动脚本 `core/examples/run.sh`。
```
