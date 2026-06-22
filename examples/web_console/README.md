# web_console —— FieldRuntime 网关 Web 管理控制台

> core 的**完整功能示例**:一套工业数据采集网关的 Web 管理控制台。
> **后端** Drogon(C++17，REST + WebSocket + 内置 ORM）+ SQLite，**内嵌 FieldRuntime 运行时**
> （复用 gateway 的 transports / assembly，无 Qt）；**前端** React + TypeScript + Vite + shadcn/ui；
> 另含一个**数据仿真服务**，无需真实硬件即可端到端演示。
>
> 设计文档：[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)（整体架构）·
> [`docs/DATA_SERVICE.md`](docs/DATA_SERVICE.md)（数据仿真字典）·
> [`docs/API.md`](docs/API.md) / [`docs/openapi.yaml`](docs/openapi.yaml)（API 参考）。

---

## 1. 它演示了什么

- **多协议南向采集**：Modbus TCP / OPC UA / Modbus RTU / S7（snap7），统一 `transport::Transport` 抽象。
- **前端动态协议配置**：协议表单由后端 `GET /transports/kinds` 的 JSON Schema 驱动，新增协议无需改前端。
- **两种取数方式**：WebSocket 订阅（实时推送）+ REST 主动拉取（最新值 / 历史）。
- **配置热生效**：改配置 → 校验 → 一键 Apply，运行时**不重启**优雅重载；支持版本与回滚。
- **协议转换**：源点位 → 变换 → 目标协议，规则可配、活动可观测。
- **鉴权 + RBAC**：JWT 登录，Viewer / Operator / Admin 三角色，按权限位放行。
- **持久化**：SQLite（配置、历史采样、事件 / 审计、用户角色）。

---

## 2. 架构总览

```
┌──────────────┐  REST + WebSocket   ┌─────────────────────────────┐
│  React SPA   │ ◀──────────────────▶│  Drogon 后端                │
│ (12 个页面)  │   /api  /ws/stream   │  HttpControllers + WsHub    │   线程安全
└──────────────┘   (vite dev 代理)    │  + 内置 ORM(SQLite)        │   SnapshotStore
                                       │  + RuntimeHost(asio 线程)  │ ◀──────┐
                                       └──────────────┬──────────────┘        │
                                                      │ 复用 FieldRuntimeBase  │ 实时快照
                                                      │ + gateway transports   │
                                       ┌──────────────▼──────────────┐        │
                                       │  Data Service(仿真)        │────────┘
                                       │  Modbus / OPC UA / S7 三协议面 │
                                       └──────────────────────────────┘
```

- **RuntimeHost**：在独立 asio io 线程托管 `GatewayAssembly`（gateway 的运行时装配），500ms 把
  datapoint / transport 快照拷进互斥保护的 store 供 REST/WS 读取（trantor ↔ asio 单写多读桥）。
- **热生效**：后端从 SQLite 配置表生成 gateway TOML → `RuntimeHost.reload()`（validate → stop → start）。

详见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。

---

## 3. 目录结构

```
examples/web_console/
├── README.md                 ← 本文
├── CMakeLists.txt            ← dataservice + backend 两个 target
├── docs/                     ← ARCHITECTURE / DATA_SERVICE / API / openapi.yaml
├── dataservice/              ← 数据仿真(field_console_dataservice)
│   ├── SimEngine.{h,cpp}     ← 9 种变化规律 + 7 种数据类型编码 + 出厂点目录
│   └── main.cpp              ← Modbus/S7/OPC UA 三协议面
├── backend/                  ← Drogon 后端(web_console_backend)
│   ├── main.cpp              ← app 启动 + schema 初始化 + 各控制器注册
│   ├── Platform.h            ← MSVC 平台垫片(htonll / NOMINMAX)
│   ├── Envelope.h            ← 统一响应包 + 行转 JSON + dp::Value→JSON
│   ├── api/auth … Controllers
│   │   ├── AuthControllers   ← 登录 / RBAC 鉴权门 / CORS
│   │   ├── ConfigControllers ← transports/datapoints/codecs/polls CRUD + kinds
│   │   ├── ConfigApply       ← 配置热生效(validate/apply/versions/rollback)
│   │   ├── DataControllers   ← 历史采样 + /data/history + /data/write
│   │   ├── ConversionEngine  ← 协议转换 CRUD + 引擎
│   │   ├── AdminControllers  ← events/audit/settings/users/roles
│   │   ├── WsControllers     ← /ws/stream 推送
│   │   └── DocsControllers   ← /api/docs(Swagger UI)
│   ├── RuntimeHost.{h,cpp}   ← 内嵌运行时 + 快照泵 + 热重载
│   ├── db/schema.sql         ← 完整数据模型 + 三角色种子
│   └── runtime.toml          ← 默认运行时接线(指向数据仿真三面)
└── frontend/                 ← React + TS + Vite + shadcn
    ├── src/app/
    │   ├── api.ts            ← API 客户端(envelope + token + openStream WS)
    │   ├── auth.ts nav.ts types.ts
    │   ├── components/       ← AppShell/Header/Sidebar/PageHeader/ui(shadcn)
    │   └── pages/            ← 12 个页面
    └── vite.config.ts        ← dev 代理 /api、/ws → 后端
```

---

## 4. 依赖

| 层 | 依赖 |
|---|---|
| 后端 | `drogon[sqlite3]`、`sqlite3`、`open62541`、`snap7`、`asio`、`tomlplusplus`（均 vcpkg）+ FieldRuntime::Base |
| 数据仿真 | `open62541`、`snap7`、`asio` + gateway nanomodbus |
| 前端 | Node ≥ 20；shadcn/radix + Tailwind + recharts + lucide-react + sonner（`npm install`） |

> `snap7` vcpkg 端口从 SourceForge 下载，国内首装较慢；首次 `vcpkg install` 可能耗时。

---

## 5. 构建

### 后端 + 数据仿真（随 `CORE_BUILD_GATEWAY` 一起，无 Qt）

```bash
cmake -S core -B out/build -G Ninja \
  -DCORE_WITH_QT=OFF -DCORE_BUILD_GATEWAY=ON \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build out/build --target web_console_backend field_console_dataservice
```

Windows（MSVC）：先导入 `vcvars64.bat` 环境再调 cmake/ninja。

### 前端

```bash
cd core/examples/web_console/frontend
npm install
npm run build      # 产物在 dist/
# 或开发模式：npm run dev
```

---

## 6. 运行

需要 3 个进程：数据仿真 → 后端 → 前端。

```bash
# 1) 数据仿真(Modbus :1502 / S7 :102 / OPC UA :4840)
field_console_dataservice 1502 127.0.0.1 4840

# 2) 后端(参数:db 路径、端口)
web_console_backend console.db 8088

# 3) 前端开发服务器(vite,代理 /api、/ws 到后端)
cd frontend && npm run dev
```

浏览器打开 vite 提示的地址（默认 `http://localhost:5173`），用默认账号登录：

| 账号 | 密码 | 角色 | 权限 |
|---|---|---|---|
| `admin` | `admin` | Admin | 全部(配置/发布/控制/用户) |
| `viewer` | `viewer` | Viewer | 只读 |

> ⚠️ **端口**：后端/前端代理示例用 8080；若本机 8080 被占用（如 Apache），请改后端端口并同步修改
> `frontend/vite.config.ts` 里 `server.proxy` 的 target（如 8088）。

生产部署：`npm run build` 产物交给后端静态托管（`web_console_backend <db> <port> <www目录>`），单进程交付。

---

## 7. 后端 API

统一响应包 `{ "code":0, "message":"ok", "data":... }`（code≠0 为错误）；鉴权
`Authorization: Bearer <token>`（`POST /api/v1/auth/login` 获取）；分页 `?page=0&size=...`。

运行时浏览：**`/api/docs`**（Swagger UI）。完整规范见 [`docs/openapi.yaml`](docs/openapi.yaml)，分类见 [`docs/API.md`](docs/API.md)。

| 分组 | 前缀 | 说明 |
|---|---|---|
| 系统 | `/system/health` `/system/info` `/system/events` `/system/settings` | 健康、构建信息、事件、设置 |
| 鉴权 | `/auth/login` `/auth/me` `/auth/logout` | 登录 / 当前用户 / 登出 |
| 协议 | `/transports` `/transports/kinds` | 协议端点 CRUD + 参数 schema |
| 采集点 | `/datapoints` `/codecs` `/poll_ranges` | 点位 / 编解码 / 轮询 CRUD |
| 转换 | `/conversions` `/conversions/{id}/stats` | 转换规则 CRUD + 统计 |
| 数据 | `/data/latest` `/data/history` `/data/write` `/runtime/transports` | 最新值 / 历史 / 控制写 / 连接状态 |
| 配置 | `/config/validate` `/config/apply` `/config/versions` `/config/versions/{v}/rollback` | 校验 / 发布 / 版本 / 回滚 |
| 用户 | `/users` `/roles` `/audit` | 用户 / 角色 / 审计 |

**WebSocket** `/ws/stream`：客户端发 `{op:"subscribe",topics:["dp/*","transport/*"]}` / `{op:"ping"}`；
服务端每秒推 `{type:"snapshot",datapoints:[{id,value,quality,ts}],transports:[{id,kind,state}]}`，慢客户端 latest-wins。

---

## 8. 前端页面（12 个，均接真后端）

| 页面 | 说明 | 数据源 |
|---|---|---|
| Dashboard 概览 | 连接墙 + KPI（5s 刷新） | `/runtime/transports` `/datapoints` |
| Live 实时监控 | 点位实时值/趋势（WS 推送，latest-wins） | `/ws/stream` `/data/latest` |
| History 历史数据 | 多点位趋势对比 + 表格 + CSV 导出 | `/data/history` |
| Protocols 协议管理 | 协议 CRUD + 动态表单 + 测试连接 | `/transports` `/transports/kinds` |
| Datapoints 采集点 | 点位 CRUD（类型/地址/编解码） | `/datapoints` |
| Polling 轮询任务 | 轮询窗口 CRUD | `/poll_ranges` |
| Conversion 协议转换 | 规则 CRUD + 启停 + 命中统计 | `/conversions` |
| Config & Apply 配置发布 | 校验 / Apply 热重载 / 版本 / 回滚 | `/config/*` |
| Logs 事件日志 | 系统事件 + 审计日志（Tabs） | `/system/events` `/audit` |
| Settings 系统设置 | 日志/保留/WS/维护 | `/system/settings` |
| Users & Roles | 用户 CRUD + 角色 + 权限矩阵 | `/users` `/roles` |
| API Docs | 内嵌 Swagger | `/api/docs` |

技术要点：协议表单由后端 JSON Schema 动态渲染；统一布局（深色侧栏 + 顶栏状态 + 白色卡片）；
角色驱动导航与按钮权限（前端仅体验，后端为最终权威）。

---

## 9. 配置热生效流程

```
在 Protocols/Datapoints/… 页编辑(写入 SQLite 草稿)
      → Config & Apply 页 Validate(POST /config/validate,丢弃式装配校验)
      → Apply(POST /config/apply):后端由 DB 生成 gateway TOML → RuntimeHost.reload()
        (validate → stop → start,进程不重启)→ 记 config_versions(active)
      → 需要时回滚:POST /config/versions/{v}/rollback
```

---

## 10. 数据仿真服务

`field_console_dataservice` 用一个 `SimEngine` 驱动三协议面产出按规律变化的数据：

- **数据类型**：bool / u16 / i16 / u32 / i32 / f32 / enum（大端）。
- **变化规律**：constant / counter / sine / triangle / sawtooth / square / random_walk / uniform_random / step_sequence（可叠高斯噪声）。
- **出厂点目录**：温度（正弦）、压力（随机游走）、运行状态（枚举轮转）、计数器、急停等，覆盖三协议。

完整数据字典见 [`docs/DATA_SERVICE.md`](docs/DATA_SERVICE.md)。生产环境用真实设备替换它，运行时/控制台零改动。

---

## 11. 实现阶段（W0–W8，均已完成并验证）

W0 数据仿真 · W1 Drogon 骨架 · W2 配置 CRUD · W3 内嵌 RuntimeHost · W4 数据 API/采样 ·
W5 WebSocket · W6 配置热生效 · W7 协议转换 · W7.6 鉴权 RBAC · W8 OpenAPI/Swagger。
前端 12 页均接通真后端 REST/WS。

---

## 12. 已知限制 / 可继续打磨

- 鉴权用 opaque token + MD5（demo）；生产应换 JWT + argon2、HTTPS（反代终结 TLS）。
- 协议转换 dest 当前走 Modbus；MQTT dest 需外部 broker。
- Permission Matrix、Settings 维护操作为前端示意，缺对应细粒度后端端点。
- 历史采样固定 2s；保留与降采样可在 Settings 配置后接调度。
- 会话/口令为内存态，多实例部署需外置（Redis 等）。

---

## 13. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| 后端启动 `Bind address failed 0.0.0.0:8080` | 端口被占（如本机 Apache）→ 换端口，并同步改 vite proxy target |
| 前端登录/接口 401 | 未带 token 或后端未启动；确认已登录、代理指向正确后端端口 |
| 后端链接 `LNK1104 无法打开 .exe` | 旧实例正在运行锁住 exe → 结束进程，或重命名旧 exe 后重链 |
| drogon 头 `htonll 找不到` | 见 `backend/Platform.h`（已在 drogon 头前提供 htonll，勿用 `/Zc:twoPhase-`，会破坏 asio） |
| `createDbClient sqlite3` FATAL | drogon 需带 `sqlite3` feature：`vcpkg.json` 写 `{"name":"drogon","features":["sqlite3"]}` |
| Live/Dashboard 无数据 | 数据仿真未启动，或运行时未连上；看 Dashboard 连接墙状态、后端日志 |
