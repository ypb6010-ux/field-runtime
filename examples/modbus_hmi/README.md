<!--
SPDX-FileCopyrightText: 2026 ypb6010-ux
SPDX-License-Identifier: MPL-2.0
-->
# modbus_hmi — Modbus 现场网关 HMI 示例(完整说明书)

一个**自包含**的 Qt6/QML 示例,演示用新 Core(FieldRuntime)搭一个最小的“现场协议网关 +
上位机”。进程内置一台**模拟 PLC**,无需任何真实硬件即可运行。

它把 Core 的四类能力做成可视化、可操作的界面:

| # | 能力 | 在 UI 上 | 背后的 Core 机制 |
|---|---|---|---|
| ① | **连接参数配置** | 改 PLC 主机/端口、操作箱端口、轮询周期 → 应用 | `[[transport]] modbus_tcp_client` + `[[poll_range]]` |
| ② | **数据点配置** | 增/删 Modbus 点(地址/类型/标度) | `[[datapoint]]` + `BuiltinScalarCodec`(标度/字序) |
| ③ | **实时数据显示** | 每个点的解码值 + 质量(Ok/Stale/…),周期刷新 | `Datapoint` 模型 + `DpChanged` + 轮询 |
| ④ | **协议转换控制** | 转发使能开关、下发设定值、模拟操作箱写入 | `modbus_tcp_server`(操作箱)+ `[[bridge]]` 整段桥接 + `[[sink_window]]` |

---

## 1. 运行

### 构建

```bat
:: 在已配置的 core 构建目录里(需 -DCORE_BUILD_EXAMPLES=ON -DCORE_BUILD_QML=ON)
cmake --build <build-dir> --target example_modbus_hmi
```

### 带窗口运行

```
! /d/developer/Qt6/JMJ/core/examples/run.sh example_modbus_hmi
```

`run.sh` 会自动配好 Qt / Core.dll / QML 路径。启动后界面分两栏:左侧①②④配置与控制,
右侧③实时数据表 + 运行日志。

### 无头自检(不弹窗,验证全链路)

```
! QT_QPA_PLATFORM=offscreen HMI_SELFTEST=1 /d/developer/Qt6/JMJ/core/examples/run.sh example_modbus_hmi
```

会脚本化跑一遍并打印:

```
SELFTEST connected=1 points=5
SELFTEST after writeSetpoint(1234): echo=1234        # 下发(Core sink)→ PLC → 回读
SELFTEST after operatorWrite(4321): echo=4321        # 操作箱写 → 桥接 → PLC → 回读
SELFTEST logs=28 status=...
```

`echo=1234 / 4321` 证明**下发链路**与**协议转换(桥接)链路**都打通了。

---

## 2. 架构

```
            ┌───────────────────────── example_modbus_hmi ─────────────────────────┐
            │                                                                       │
  QML UI ──▶│  GatewayController (QObject, 稳定)                                     │
  (Main.qml)│    • 连接/数据点配置状态  → buildToml() 生成 TOML                       │
            │    • apply(): 销毁旧 Core → 新建 ICore → loadConfig → start            │
            │    • 250ms 定时器 pull 各 Datapoint 当前值 → points 属性(QML 绑定)    │
            │    • forwarding 开关 → setServerForwardEnabled                         │
            │    • writeSetpoint / simulateOperatorWrite                             │
            │            │                                                          │
            │            ▼                                                          │
            │      core::ICore ──┬─ modbus_tcp_client "plc"  ──────────────┐        │
            │                    ├─ modbus_tcp_server "opbox"(操作箱)      │        │
            │                    ├─ poll_range / datapoints / sink_window   │        │
            │                    └─ bridge: opbox ↔ plc(整段桥接)          │        │
            └────────────────────────────────────────────────────┼────────┘        │
                                                                  │                 │
                                                  ┌───────────────▼──────────────┐  │
                                                  │ SimulatedPlc(独立线程)         │  │
                                                  │ QModbusTcpServer @127.0.0.1   │  │
                                                  │ HR0..7 正弦信号;HR8..31 可写  │  │
                                                  └───────────────────────────────┘  │
```

**为什么 QML 绑到 Controller 而不是 Core?** Controller 是稳定对象;`apply()` 会把内部
`ICore` 整个换掉(实现“改配置→重启运行时”)。QML 只绑 Controller 的属性,Core 热替换对 UI 透明。

**为什么模拟 PLC 在独立线程?** Core 的一次轮询是同步阻塞调用,会占住调用线程直到读应答
返回。若响应方(模拟 PLC)与轮询在同一线程,就永远应答不了 → 全部超时。所以 `SimulatedPlc`
把 `QModbusTcpServer` 放在自己的 `QThread`。

---

## 3. 它生成的配置(buildToml)

点「应用」时,Controller 用当前 UI 状态拼出一份 TOML(写到临时文件再 `loadConfig`)。等价于:

```toml
[meta] project = "modbus_hmi"

[[transport]]                      # ① 连接参数来自 UI
id = "plc"
kind = "modbus_tcp_client"
host = "127.0.0.1"; port = 5602; slave_id = 1
[transport.scheduler] kind = "serial"; inter_request_gap_ms = 5

[[transport]]                      # ④ 操作箱 = Modbus server
id = "opbox"
kind = "modbus_tcp_server"
listen_port = 5603
[[transport.listen_ranges]] table = "HR"; range = [0, 32]

[[poll_range]]                     # ① 轮询周期来自 UI
module_id = "poll.plc"; transport = "plc"; table = "HR"; range = [0, 32]; period_ms = 500

[[sink_window]]                    # ④ 下发设定值的写窗口
module_id = "sw.cmd"; transport = "plc"; table = "HR"; range = [20, 2]; priority = "High"

[[datapoint]]                      # ② 每个配置点(标度/字序由内置标量 codec 处理)
id = "temperature"; kind = "Status"; type = "S16"
source = { port = "plc", table = "HR", addr = 0, scale = 0.1 }
# ... belt_speed / pressure / current ...

[[datapoint]]                      # ④ 下发命令点(经 sink window 写 PLC)
id = "setpoint"; kind = "Command"; type = "U16"
sink = { port = "plc", table = "HR", addr = 20, window = "sw.cmd" }

[[datapoint]]                      # ④ 回读点(从 PLC 读回设定值寄存器)
id = "setpoint_echo"; kind = "Status"; type = "U16"
source = { port = "plc", table = "HR", addr = 20 }

[[bridge]]                         # ④ 整段桥接:操作箱 ↔ PLC
server = "opbox"; plc = "plc"; offset = 0
write_start = 20; write_count = 2   # 操作箱写 HR20..21 → 转发到 PLC
mirror_start = 0; mirror_count = 8  # PLC HR0..7 → 周期镜像回操作箱(操作箱能读到实时数据)
mirror_period_ms = 200
```

---

## 4. 协议转换控制 ④ 的两条链路

**下发(上位机直接控制)**:`writeSetpoint(v)` → `Datapoint("setpoint").write(v)` →
Core 把值编码后 stage 进 `sink_window "sw.cmd"` → TickDriver 刷写到 PLC `HR20`。
`setpoint_echo` 下个轮询读回 → UI 显示。

**协议转换(操作箱 → PLC)**:`simulateOperatorWrite(v)` 用一个内部 `QModbusTcpClient`
连上**操作箱**(opbox server),写 `HR20=v`。Core 的 server transport 发出 `ServerWriteEvent`
→ `[[bridge]]` 把写区 `HR20..21` 转发到 PLC(地址 = server 地址 − offset)→ `setpoint_echo`
读回。**“转发使能”开关**(`setServerForwardEnabled("opbox", …)`)就是这条转发链路的业务闸门:
关掉后,操作箱的写入在程序内被拦截,不会下发到 PLC(可在 UI 上对比验证)。

同时,桥接的**镜像方向**(PLC HR0..7 → 操作箱寄存器表)让真实操作箱无需直连 PLC 就能读到实时
数据——这正是“协议转换网关”的典型用法。

---

## 5. 文件一览

| 文件 | 作用 |
|---|---|
| `main.cpp` | 起模拟 PLC、建 Controller、装 UI 日志 sink;GUI 模式加载 QML,自检模式脚本化跑全链路 |
| `GatewayController.{h,cpp}` | 核心 view-model:配置状态、`buildToml`、`apply`(热替换 Core)、pull 刷新、控制动作 |
| `SimulatedPlc.{h,cpp}` | 独立线程的 `QModbusTcpServer`,HR0..7 造正弦数据,HR8..31 可写回读 |
| `UiLogSink.h` | 自定义 `ILogSink`,把 Qt-free 的日志记录(std::string/chrono)转 QVariantMap 喂给 QML |
| `qml/Main.qml` | 四区界面:①连接 ②数据点 ③实时表 ④协议转换控制 + 日志 |

---

## 6. 你可以怎么扩展

- **真实 PLC**:把①的主机/端口改成现场 PLC,去掉 `SimulatedPlc`,直接连。
- **更多点位**:②里加点(支持 U16/S16/U32/S32/F32;多寄存器类型自动补 `wordOrder=ABCD`)。
- **RTU / OPC UA / MQTT**:`buildToml` 里把 `kind` 换成 `modbus_rtu` / `opc_ua_client` /
  `mqtt_client`,Core 同一套 datapoint/bridge 模型不变(这正是 FieldRuntime 的价值)。
- **持久化**:挂上 `CorePersistence`,给 datapoint 配 `persist = "tag"`,历史落库。
