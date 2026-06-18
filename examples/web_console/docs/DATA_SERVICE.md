# Data Service —— 数据仿真服务 · 数据字典与设计

> example `web_console` 的配套组件(D8)。独立可执行 `field_console_dataservice`(无 Qt)。
> **目的**:无需真实硬件,即可让 `Data Service → FieldRuntimeBase → drogon → React` 整链跑通、
> 看到按规律变化的数据。本文是**数据契约**:数据类型、变化规律(pattern)、默认点目录、配置格式。
> 架构定位见 [`ARCHITECTURE.md`](./ARCHITECTURE.md) §7.5。状态:设计稿 v0.1。

---

## 1. 形态与时基

- **四协议面**(同进程,各自监听),供运行时当"真设备"接入(对齐 console 本期协议范围 D9):
  - **Modbus TCP server**:持有 HR/IR 寄存器表,按规律刷新;运行时 `modbus_tcp_client` 轮询。
  - **OPC UA server**:`ns=2;s=Sim_*` 节点;运行时 `opc_ua_client` 读。
  - **MQTT publisher**:向 broker 周期发 `sim/<id>`(JSON 或 raw);运行时 `mqtt_client` 订阅。
  - **S7 server**(snap7 server):持有 DB 区(如 DB1)字节块,按规律刷新;运行时 `s7_client` 读
    `DB<n>.<offset>`。(S7 是 console 本期新增南向,见 ARCHITECTURE §D9/W7.5。)
- **SimEngine**:统一的"点"求值器。每点有自己的 `update_period_ms`;到点时按 **pattern + 墙钟时刻 `t`**
  重算值,写入对应协议面的存储。时间型 pattern(sine 等)用墙钟 `t`,**与刷新率解耦**——刷新越快只是采样越密,波形不变。
- **复用**:在现有 `MockModbusTcpServer` / `OpcUaMockServer` / `MqttMockBroker` / RTU mock 之上,把"固定值"换成 SimEngine 驱动。

```
sim.toml ─▶ SimEngine ─┬─▶ Modbus regs ─▶ Modbus TCP server :1502
        (pattern求值)   ├─▶ OPC UA nodes ─▶ OPC UA server   :4840
                        └─▶ MQTT payloads ─▶ publish sim/*   broker:1883
```

---

## 2. 数据类型(data types)

| type | 位宽 | Modbus 占用 | 编码 | 取值范围 | OPC UA | S7 | MQTT(JSON) |
|---|---|---|---|---|---|---|---|
| `bool` | 1 | 1 寄存器(0/1) | 0/非0 | {0,1} | Boolean | Bool(`DBx.DBXn.b`) | `true/false` |
| `u16` | 16 | 1 | 无符号 | 0 … 65535 | UInt16 | Word | number |
| `i16` | 16 | 1 | 补码 | −32768 … 32767 | Int16 | Int | number |
| `u32` | 32 | 2 | 字序 hi/lo(可配) | 0 … 4.29e9 | UInt32 | DWord | number |
| `i32` | 32 | 2 | 补码 + 字序 | −2.15e9 … 2.15e9 | Int32 | DInt | number |
| `f32` | 32 | 2 | IEEE-754 + 字序 | ±3.4e38 | Float | Real | number |
| `enum` | 16 | 1 | u16 离散码 + 标签 | 离散集 | Int32(码) | Int | string(标签)/number(码) |

> **S7 字节序**:S7 为大端;`DBx.DBDn`(DWord/DInt/Real)按大端存放。`DBx.DBXn.b` 是位寻址(字节 n 的第 b 位)。

**字序(word_order)**:32 位类型跨两个寄存器,默认 `hi_lo`(大端字序),可per-point 配 `lo_hi`;
要与 console 侧 datapoint 的 `word_order` 一致。**raw vs 工程值**:SimEngine 产出**原始寄存器/节点值**,
工程换算(scale/codec)由 console 的 datapoint 做——和真实设备一致(sim 给 raw 250,console scale 0.1 → 25.0)。

---

## 3. 变化规律(pattern)

> 设 `t` = 墙钟自启动秒数,`T` = `period_ms/1000`,`φ` = `phase`(弧度,默认 0)。
> 数值 pattern 算出**浮点 v**,再按点的 `type` 量化/编码;可叠加高斯噪声。

| pattern | 公式 / 行为 | 参数 | 适用类型 |
|---|---|---|---|
| `constant` | `v = value` | `value` | 全 |
| `counter` | 每次刷新 `v += step`,到 `wrap` 归零 | `start, step, wrap` | u16/u32/i32 |
| `sine` | `v = offset + amplitude·sin(2π·t/T + φ)` | `offset, amplitude, period_ms, phase` | i16/f32/… |
| `triangle` | 在 `[min,max]` 间线性往返,周期 `T` | `min, max, period_ms` | 数值 |
| `sawtooth` | `min→max` 线性上升后跳回,周期 `T` | `min, max, period_ms` | 数值 |
| `square` | 前 `duty` 段为 `max`,其余 `min`,周期 `T` | `min, max, period_ms, duty(0–1)` | bool/数值 |
| `random_walk` | `v += U(−step, +step)`,clamp 到 `[min,max]` | `start, step, min, max, seed` | 数值 |
| `uniform_random` | `v = U(min, max)`,每次刷新独立 | `min, max, seed` | 数值 |
| `step_sequence` | 依次取 `values[i]`,每个停留 `dwell_ms` 后切下一个,循环 | `values[], dwell_ms` | enum/数值 |

**噪声叠加(可选)**:任意数值 pattern 加 `noise_sigma` → `v += N(0, sigma)`。
**量化规则**:整型 = `round(clamp(v, type_min, type_max))`;`bool` = `v ≥ 0.5`;
`enum` 取 `values` 里的离散码;`f32` 直接编码。

---

## 4. 默认仿真点目录(sim.toml 自带)

> 覆盖 **三协议 × 各数据类型 × 各 pattern**,与 console 默认配置一一对应——一键起 sim+console 即见动态。

### 4.1 Modbus TCP(`:1502`,unit 1,HR)

| id | addr | type | pattern | 关键参数 | 现象(console 侧) |
|---|---|---|---|---|---|
| `sim.temperature` | HR0 | i16 | sine | offset=250 amp=80 T=30s | scale 0.1 → 17.0–33.0 °C 正弦 |
| `sim.pressure` | HR1 | u16 | random_walk | start=5000 step=50 [4000,6000] | scale 0.001 → ~5 bar 抖动 |
| `sim.flow` | HR2–3 | f32 | sine | offset=120 amp=40 T=20s | 80–160 m³/h |
| `sim.run_state` | HR4 | enum | step_sequence | values=[0,1,1,1,2] dwell=5s | stopped/running/fault 轮转 |
| `sim.total_count`| HR5–6 | u32 | counter | start=0 step=1 wrap=1e6 | 单调累加 |
| `sim.alarm` | HR7 | bool | square | T=60s duty=0.1 | 偶发报警 |

### 4.2 OPC UA(`opc.tcp://:4840`,`ns=2`)

| id | node | type | pattern | 关键参数 |
|---|---|---|---|---|
| `sim.opc.level` | `Sim_Level` | f32 | triangle | [0,100] T=40s |
| `sim.opc.speed` | `Sim_Speed` | i32 | random_walk | start=1450 step=20 [1000,1800] |
| `sim.opc.valve` | `Sim_Valve` | bool | square | T=30s duty=0.5 |

### 4.3 MQTT(publish `sim/<id>`,JSON)

| id | topic | type | pattern | 关键参数 |
|---|---|---|---|---|
| `sim.mqtt.power` | `sim/power` | f32 | sine | offset=75 amp=25 T=25s(kW) |
| `sim.mqtt.status` | `sim/status` | enum | step_sequence | values=["idle","load","peak"] dwell=8s |
| `sim.mqtt.vibration` | `sim/vib` | f32 | uniform_random | [0.1,2.5] + noise_sigma=0.05 |

### 4.4 S7(snap7 server,DB1)

| id | 地址 | type | pattern | 关键参数 |
|---|---|---|---|---|
| `sim.s7.motor_temp` | `DB1.DBW0` | i16 | sine | offset=400 amp=120 T=35s(×0.1=40±12℃) |
| `sim.s7.torque` | `DB1.DBD2` | f32 | random_walk | start=85 step=3 [40,140] N·m |
| `sim.s7.cycle` | `DB1.DBD6` | u32 | counter | start=0 step=1 wrap=1e7 |
| `sim.s7.estop` | `DB1.DBX10.0` | bool | square | T=120s duty=0.05(偶发急停) |

---

## 5. 配置格式(`sim.toml`)

```toml
[meta]
name = "field_console_sim"
default_update_period_ms = 500          # 点未指定时的刷新周期

[modbus]
listen   = "0.0.0.0:1502"
unit_id  = 1

[[modbus.point]]
id = "sim.temperature"
table = "HR"; addr = 0
type = "i16"
pattern = "sine"
offset = 250; amplitude = 80; period_ms = 30000

[[modbus.point]]
id = "sim.run_state"
table = "HR"; addr = 4
type = "enum"
pattern = "step_sequence"
values = [0, 1, 1, 1, 2]; dwell_ms = 5000

[[modbus.point]]
id = "sim.total_count"
table = "HR"; addr = 5
type = "u32"; word_order = "hi_lo"
pattern = "counter"
start = 0; step = 1; wrap = 1000000

[opcua]
endpoint  = "opc.tcp://0.0.0.0:4840"
namespace = "http://fieldruntime/sim"   # → ns=2

[[opcua.point]]
id = "sim.opc.level"
node = "Sim_Level"
type = "f32"
pattern = "triangle"
min = 0; max = 100; period_ms = 40000

[mqtt]
broker = "tcp://127.0.0.1:1883"         # 外部 broker;或 built_in=true 用内置 mock broker
qos = 0

[[mqtt.point]]
id = "sim.mqtt.power"
topic = "sim/power"
type = "f32"; encoding = "json"          # json | raw
pattern = "sine"
offset = 75; amplitude = 25; period_ms = 25000
update_period_ms = 1000                  # 覆盖默认

[s7]
listen = "0.0.0.0:102"                   # snap7 server,ISO-on-TCP/RFC1006
rack = 0; slot = 1
db = 1                                    # 提供的 DB 区号

[[s7.point]]
id = "sim.s7.torque"
addr = "DB1.DBD2"                         # 大端 Real
type = "f32"
pattern = "random_walk"
start = 85; step = 3; min = 40; max = 140

[[s7.point]]
id = "sim.s7.estop"
addr = "DB1.DBX10.0"                      # 位寻址
type = "bool"
pattern = "square"
period_ms = 120000; duty = 0.05
```

> console 可通过专门接口(`POST /api/v1/dev/sim/*`,仅开发模式)在线下发/修改 sim.toml,
> 方便在 UI 里直接调仿真——这是开发便利,非生产接口。

---

## 6. 异常注入(可选,验证质量与重连)

按点或全局注入,用来验证 console 侧的 `quality`、transport 重连、conversion 错误计数:

| 注入 | 行为 | 验证点 |
|---|---|---|
| `disconnect` | 关闭某协议面监听一段时间 | transport 状态→error→重连 |
| `timeout` | 收到请求不回包 | 读超时(RTU/TCP deadline)、quality=bad |
| `bad_value` | 产出 NaN / 越界 / 坏字序 | datapoint quality、UI 异常态 |
| `freeze` | 停止刷新,值不再变 | "数据陈旧"判定(可选 staleness) |

接口(开发模式):`POST /api/v1/dev/sim/fault {point|face, kind, duration_ms}`。

---

## 7. 运行方式

```bash
# 1) 起仿真(三协议面)
field_console_dataservice --config sim.toml
#   mock OPC UA on opc.tcp://0.0.0.0:4840
#   modbus sim server on 0.0.0.0:1502 (unit 1)
#   mqtt publishing sim/* to tcp://127.0.0.1:1883
#   S7 sim server on 0.0.0.0:102 (rack 0 slot 1, DB1)

# 2) 起 console 后端(默认配置已指向上面三个面)
web_console --db console.db --www ./www

# 3) 浏览器打开 console → Live 页即见 sim.* 点位按规律跳动;
#    Conversion 页可配 sim.temperature → MQTT 验证转换。
```

---

## 8. 实现要点(给后续编码)

- SimEngine 与协议面**同 io_context 单线程**(沿用 gateway 模型);每点一个
  `update_period_ms` 定时器或统一 tick。
- pattern 求值是纯函数 `eval(pattern, t, state) -> double`(`counter/random_walk/step_sequence` 带状态),
  便于**单测**(给定 t 序列断言波形)——对齐 gateway 的"帧自测"风格。
- 编码层复用 console/runtime 的 `dp` 编解码工具(u32 字序、f32、enum),避免两套实现漂移。
- 默认点目录里的地址/类型/codec 要与 `web_console` 的**默认 DB 配置**严格对应(同一份"出厂配置")。
