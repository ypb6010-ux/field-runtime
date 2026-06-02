# qml_dashboard — 日志与数据库模块 QML 演示

一个完整展示新 Core **日志系统**与**数据库持久化模块**的 QML 面板,风格类似 Qt 自带示例。
内置一个**进程内模拟 PLC**(Modbus TCP server,127.0.0.1:5502),所以无需任何真实硬件即可看到实时数据。

## 三个标签页

### 数据源 Sources
- **Transports**:列出所有传输,每个带 **数据来源种类徽章**(Modbus TCP / MQTT / OPC UA)与连接状态、调度队列深度、p99 延迟。
  - `plc1`(Modbus TCP)连接到模拟 PLC,显示 *Connected*。
  - `mqtt1`(MQTT)、`opc1`(OPC UA)指向不存在的服务端,显示 *Disconnected* —— 用来演示**多来源标识**;它们的连接失败也会出现在日志流里。
- **Datapoints**:每个数据点显示实时值、状态,并**明确标注数据来源**(Modbus TCP)。`run_state` 通过 EnumU16 codec 解码为文字。

### 日志 Logs
- **级别阈值** ComboBox → 实时调整 `Logger` 阈值。
- **运行日志(审计)** 按钮(启动/停止/复位)→ 通过 `demo.emitOperation` 产生 `OperationRecord`(actor=`ui:user`),**不受级别过滤**。
- **UI 警告** 按钮 → 直接调用注入到 QML 的 `log` 桥(`log.warn(...)`),产生系统日志。
- 实时日志流:系统日志按级别着色,运行日志以紫色 `OP` 徽章区分;右上角显示**过载丢弃计数**。
- 日志来自一个自定义 `UiLogSink`(演示 `ILogSink` 可注册扩展点),在 logger 分发线程上以 queued 调用安全地送进 UI。

### 历史 History(需 Postgres)
- 选择 Telemetry / Operation / System,填时间范围与页码,**分页查询**持久化数据。
- 采集数据来自各数据点的 `persist` 标签;运行/系统日志由 `DbLogSink` 落库。
- 若启动时连不上 Postgres,本页显示"数据库不可用",其余两页照常工作。

## 构建

```
cmake -S core -B core/build -DCORE_BUILD_PERSISTENCE=ON ^
  -DQxOrm_DIR=D:/developer/3rdparty/QxOrm/6.8.3/cmake
cmake --build core/build --target example_qml_dashboard
```

> 不开 `CORE_BUILD_PERSISTENCE` 也能构建运行(History 页禁用)。

## 运行

需要 Qt 的 QML 模块、QPSQL 驱动依赖(`libpq.dll`)在 PATH 上。Postgres 连接默认
`localhost:5432` 库 `jmj_core`(自动创建),用户 `postgres`。配置见 `dashboard.toml`。
