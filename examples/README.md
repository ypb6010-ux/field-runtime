# Core 示例 — 运行指南

用统一启动脚本跑(自动配好 Qt / DLL / QML / libpq 路径)。从 Claude 的 `!` 提示符或 Git Bash:

```
! /d/developer/Qt6/JMJ/core/examples/run.sh <exe 名> [参数...]
```

Qt 路径默认 `D:/developer/3rdparty/Qt/6.8.3/msvc2022_64`,不对就先 `! export QT6_BIN="D:/你的Qt"`。

## 可独立运行(无需外部 server / 硬件)

| 示例 | 命令 | 说明 |
|---|---|---|
| **QML 面板** | `run.sh example_qml_dashboard` | 日志系统 + 数据库 + 多来源(Modbus/MQTT/OPC UA)的 QML 演示,内置模拟 PLC。带窗口 GUI。History 页需本地 Postgres(`jmj_core`,自动建表);不起也能看前两页。 |
| **延迟诊断** | `run.sh diag_modbus_latency --inproc --port 5599` | 对比"主线程 vs worker 线程"的 modbus 读延迟(同进程内嵌 server)。控制台输出。 |
| 延迟诊断(独立 server) | `run.sh diag_modbus_latency --serve --port 5599`<br>另开一个:`run.sh diag_modbus_latency --host 127.0.0.1 --port 5599` | 跨进程对照 |

> 构建:`cmake --build core/build --target example_qml_dashboard diag_modbus_latency`
> (QML 面板需配置时带 `-DCORE_BUILD_PERSISTENCE=ON -DQxOrm_DIR=.../QxOrm/6.8.3/cmake`)

## 需要外部 Modbus server / 配置(非开箱即跑)

`minimal_modbus`、`operator_box_to_plc`、`stats_dashboard` 是控制台示例,需要你自己起一个
Modbus TCP server 或操作箱模拟器,并传入对应 `*.toml`。各子目录有独立 README。

## 无头自检(不弹窗,快速验证)

QML 面板支持无头自检:跑 ~4 秒后打印调度器 p50/p99 与落库计数再退出:

```
! QT_QPA_PLATFORM=offscreen DASHBOARD_SELFTEST=1 /d/developer/Qt6/JMJ/core/examples/run.sh example_qml_dashboard
```
