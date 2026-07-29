# 数据仿真服务

`field_console_dataservice` 是 Web Console 的无硬件验证程序。一个 `SimEngine` 同时驱动
Modbus TCP、S7 和 OPC UA 三个真实协议服务端；当前没有 MQTT face。

## 启动

```text
field_console_dataservice [modbus-port] [s7-bind-address] [opcua-port]
```

默认值：

| 协议 | 地址 | 数据区 |
|---|---|---|
| Modbus TCP | `0.0.0.0:1502` | unit 1，Holding Register |
| S7 | `0.0.0.0:102` | DB1 |
| OPC UA | `opc.tcp://0.0.0.0:4840` | namespace `http://fieldruntime/sim` |

程序每 200 ms 更新一次 Modbus 和 S7 缓冲区；OPC UA 在服务器线程中以同样周期更新。
Ctrl+C/SIGTERM 会停止三种协议并等待线程退出。Modbus 客户端连接采用非阻塞读写，因此空闲连接
不会阻止进程关闭。

## 内置点位

### Modbus TCP

地址是 Holding Register 的零基 word index；32 位值按高字在前编码。

| id | 地址 | 类型 | 规律 |
|---|---:|---|---|
| `sim.temperature` | 0 | I16 | 正弦，250±80；常用 scale=0.1 |
| `sim.pressure` | 1 | U16 | 4000..6000 随机游走 |
| `sim.flow` | 2..3 | F32 | 正弦，120±40 |
| `sim.run_state` | 4 | enum/U16 | 每 5 秒按 0,1,1,1,2 轮转 |
| `sim.total_count` | 5..6 | U32 | 递增，1000000 回绕 |
| `sim.alarm` | 7 | bool/U16 | 60 秒方波，10% 占空比 |

支持：

- `0x03` Read Holding Registers
- `0x04` Read Input Registers（映射到同一仿真缓冲）
- `0x10` Write Multiple Registers

请求长度、寄存器范围、数量和 byte count 都经过校验；非法请求返回标准 Modbus exception。
仿真 ticker 后续可能覆盖写入到出厂点位地址的值。

### S7

地址在代码中以 word index 表示，对外是 `DB1` 的 `addr * 2` 字节偏移。

| id | word 地址 | 类型 | 规律 |
|---|---:|---|---|
| `sim.s7.motor_temp` | 0 | I16 | 正弦，400±120 |
| `sim.s7.torque` | 1..2 | F32 | 40..140 随机游走 |
| `sim.s7.cycle` | 3..4 | U32 | 递增，10000000 回绕 |

S7 由 snap7 server 托管，绑定地址由第二个命令行参数指定。

### OPC UA

namespace index 由服务器启动时分配，NodeId 使用字符串 `Sim_<addr>`。

| id | NodeId suffix | UA 类型 | 规律 |
|---|---:|---|---|
| `sim.opc.level` | `Sim_0` | Float | 0..100 三角波，40 秒 |
| `sim.opc.speed` | `Sim_1` | Int32 | 1000..1800 随机游走 |
| `sim.opc.valve` | `Sim_2` | Boolean | 30 秒方波，50% 占空比 |

节点只读。

## SimEngine 能力

支持的原始类型：

- Bool、U16、I16
- U32、I32、F32
- Enum（以 U16 编码）

支持的变化规律：

- constant
- counter
- sine
- triangle
- sawtooth
- square
- random walk
- uniform random
- step sequence

部分规律支持 offset、amplitude、min/max、period、step、wrap、duty、dwell 和噪声参数。
当前目录由 `SimEngine::defaultCatalog()` 固定构造，未从外部配置文件加载；修改点位时应同步更新
本文和 Web Console 的示例 runtime 配置。

## 验证建议

1. 启动 dataservice 后分别使用 Modbus、S7、OPC UA 客户端读取上述点位；
2. 启动 Web Console backend，确认三种 transport 进入 connected；
3. 验证 Live 的值随时间变化，History 不重复写入相同时间戳；
4. 保持一个 Modbus TCP 空闲连接后发送 Ctrl+C，确认进程能及时退出；
5. 向 0x10 发送错误 byte count/截断 PDU，确认收到 exception 且服务不崩溃。
