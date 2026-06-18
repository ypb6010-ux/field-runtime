# FieldRuntime Console —— Web 网关管理控制台 · 技术架构设计

> example 名:`web_console` · 后端 Drogon(C++) + SQLite + ORM · 前端 React
> 目标:给现有无 Qt 的 **FieldRuntime 运行时**(transport / datapoint / codec /
> scheduler / bridge)套一层 Web 控制台:协议配置、消息订阅(WebSocket)与主动拉取
> (REST)、配置热生效、协议转换,全部可在前端 UI 配置。
> 状态:**设计稿 v0.1**(2026-06-18)。前端页面由用户用 Figma 设计,后端接口与整体
> 架构由本设计定义。

---

## 0. 关键决策(请先确认 / 可推翻)

| # | 决策 | 取值 | 理由 / 备选 |
|---|---|---|---|
| D1 | 运行时来源 | ✅**复用 `FieldRuntimeBase` + gateway 的 asio transports** | 已确认。不重写 Modbus/OPC UA/MQTT,与 gateway 共享 transports 静态库 |
| D2 | "热更新"含义 | ✅**配置热生效**(运行时优雅重配,进程不重启) | 已确认。插件 DLL 热替换留作 P2 |
| D8 | 显示验证数据源 | ✅**独立的数据仿真服务(Data Service / Mock)**,走 Modbus/OPC UA/MQTT 喂数据 | 已确认。无需真硬件即可端到端验证;数据类型 + 变化规律单独成文档(见 `DATA_SERVICE.md` / §7.5) |
| D3 | ORM | **Drogon 内置 ORM**(`DbClient` + `Mapper<Model>`,sqlite3) | 与 drogon 一体、可由 `drogon_ctl` 由 schema 生成 |
| D4 | 两循环桥接 | 运行时跑独立 asio io 线程;drogon 跑 trantor 线程池;**跨线程用 `loop->queueInLoop` 单向投递** | 见 §4 |
| D5 | 鉴权 + 权限 | ✅**强制鉴权 + RBAC**:JWT 登录,角色 Viewer/Operator/Admin,**单套前端 + 角色工作区**(非两套系统) | 已确认。详见 §12。物理隔离用反代/构建开关,不分叉代码 |
| D6 | 前端栈 | React + TS + Vite + Ant Design + React Query + 一个 WS client + ECharts | 表单/表格/数据密集场景成熟;**协议表单由后端 JSON Schema 驱动动态渲染** |
| D7 | 仓库位置 | ✅`core/examples/web_console/`,作为 **core 完整功能示例**;CMake `BUILD_WEB_CONSOLE` 默认 OFF | 已确认 |
| D9 | 协议范围(本期) | ✅**Modbus TCP · OPC UA · MQTT · S7** 四种;**RTU 本期不纳入** console | 已确认。⚠️**S7 运行时是 stub,需新写 S7 客户端(运行时)+ S7 服务端面(仿真),拟用 snap7** |
| D10 | 历史保留 | ✅**固定 30 天 + 可配置的定期清理**任务 | 已确认。`settings.sample_retention_days=30` + 后台清理调度,见 §6.1 |

> 决策若要改,告诉我对应编号。**S7(D9)是本期唯一"需新增南向实现"的项**,工作量同 OPC UA/RTU。

---

## 1. 目标与范围

**协议范围(本期,D9)**:Modbus TCP · OPC UA · MQTT · **S7**(S7 需新增运行时实现)。
RTU 本期不纳入 console(gateway 仍有)。

**做什么**
- 在前端**配置每种协议**(Modbus TCP、OPC UA、MQTT、S7)的连接与采集点。
- **两种取消息方式**:WebSocket 订阅(实时推送)+ REST 主动拉取(快照 / 历史)。
- **API 分类 + 文档**:系统配置接口、协议配置接口、数据接口、转换接口,OpenAPI/Swagger。
- **配置热生效**:改完配置后,前端有显式"应用/使其生效"入口,后端优雅重配。
- **协议转换**:源协议点 → 目标协议点的映射(含变换),前端可配置可观测。

**不做(本期)**
- 多租户 / 复杂 RBAC;二进制热补丁;分布式集群;TLS 终结(交给反向代理)。

---

## 2. 系统上下文

```
        ┌─────────────────────────── FieldRuntime Console ───────────────────────────┐
        │                                                                             │
  ┌─────────────┐   HTTP/REST     ┌──────────────────┐                                │
  │             │ ───────────────▶│  Drogon HTTP      │   thread-safe                  │
  │  React SPA  │   WebSocket     │  Controllers      │   command queue   ┌──────────┐ │
  │ (Figma 设计)│ ◀──────────────▶│  + WS Hub         │ ─────────────────▶│ Runtime  │ │
  │             │                 │  + Drogon ORM     │ ◀──── events ─────│  Host    │ │
  └─────────────┘                 └────────┬─────────┘   (SnapshotStore /  │ (asio io)│ │
                                           │              WsHub push)       └────┬─────┘ │
                                      ┌────▼────┐                                │       │
                                      │ SQLite  │                     FieldRuntimeBase  │
                                      │ (config │                     + asio transports │
                                      │ +history│              Modbus TCP/RTU · OPC UA · │
                                      └─────────┘                  MQTT · Conversion     │
        └─────────────────────────────────────────────────────────────────────┬───────┘
                                                                               │ 南向(Modbus/OPC UA/MQTT)
                              ┌────────────────────────────────────────────────▼───────┐
   验证回路:Data Service →   │  Data Service(数据仿真 / Mock,见 §7.5 + DATA_SERVICE.md) │
   FieldRuntimeBase →        │  Modbus TCP server · OPC UA server · MQTT publisher       │
   drogon → React            │  按"数据类型 + 变化规律"产生仿真数据(sine/ramp/enum…)    │
                              └──────────────────────────────────────────────────────────┘
            生产环境则把 Data Service 换成真实 现场设备 / PLC / 操作箱 / Broker。
```

- **Drogon 层**:HTTP 控制器(REST)、WebSocket 控制器与推送 Hub、ORM 读写 SQLite。
- **Runtime Host**:拥有一个 asio `io_context` + 线程,内部装配 = 现有 `GatewayAssembly`
  的演化(由 DB 配置构建 transports / datapoints / polls / bridges / conversions)。
- **SQLite**:配置(草稿/生效/版本)+ 历史采样 + 事件日志。

---

## 3. 总体分层

```
┌─ 表现层  React SPA ──────────────────────────────────────────────┐
│  Dashboard / Protocols / Datapoints / Conversion / Live / History │
│  / Config&Apply / Settings / API Docs / Logs                       │
├─ 接入层  Drogon ─────────────────────────────────────────────────┤
│  HttpControllers(REST) · WebSocketController + WsHub · 静态资源    │
│  Filters(Auth / CORS / 请求日志)                                  │
├─ 应用服务层  (drogon 线程内,纯逻辑)─────────────────────────────┤
│  ConfigService(草稿/校验/应用/回滚/版本) · TransportService ·      │
│  DatapointService · ConversionService · DataQueryService           │
├─ 持久层  Drogon ORM ─────────────────────────────────────────────┤
│  Models(transports/datapoints/codecs/polls/conversions/           │
│  config_versions/samples/events/settings) + DAO                    │
├─ 运行时桥  RuntimeHost ──────────────────────────────────────────┤
│  CommandQueue(drogon→runtime) · SnapshotStore(latest 值) ·         │
│  RuntimeBridge(EventBus→WsHub) · ConfigApplier(优雅重配)          │
├─ 运行时核心  FieldRuntimeBase + gateway/asio ────────────────────┤
│  Transport(TCP/RTU/OPC UA/MQTT) · Scheduler · Datapoint 引擎 ·     │
│  Codec(enum/lua) · Bridge · ConversionEngine · EventBus            │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. 线程与事件循环模型(核心难点)

drogon 用 **trantor** 事件循环(主 loop + N 个 IO worker loop);运行时用 **asio**
`io_context`(单线程,沿用 gateway 模型)。两套 reactor,必须明确边界,**禁止跨循环直接调用对方对象**。

**两条单向通道:**

1. **下行(drogon → runtime)** —— 配置变更、控制写、立即采样等"命令":
   drogon 控制器把命令 `asio::post(runtimeIo, cmd)` 投到运行时线程执行;结果通过
   `std::promise`/回调或写回 SnapshotStore 返回。控制器侧若需同步响应,等 future(注意:
   drogon 线程可阻塞等,但**不可**阻塞 runtime io 线程本身)。

2. **上行(runtime → drogon)** —— datapoint 变化、连接状态、转换活动、事件:
   - 运行时 `EventBus` 发布变化 → `RuntimeBridge` 接收(运行时线程内)。
   - **写 `SnapshotStore`**(线程安全的 latest-value 表)供 REST 拉取。
   - **推 `WsHub`**:对每个订阅连接 `wsConn->getLoop()->queueInLoop([=]{ wsConn->send(json); })`,
     把发送切回该连接所属 trantor loop,零锁竞争。

**SnapshotStore**:`unordered_map<dpId, {value, raw, ts, quality}>`,用
`std::shared_mutex` 或 `atomic<shared_ptr<const Snapshot>>`(RCU 风格,读多写少)。
运行时单写,drogon 多读。这层把"实时推送"和"按需拉取"彻底解耦。

```
现场→Transport→Datapoint引擎→EventBus──┬──▶ SnapshotStore ──(读)──▶ REST /data/latest
                                        └──▶ RuntimeBridge ──queueInLoop──▶ WsHub ──▶ WS 订阅者
HTTP 控制器 ──CommandQueue(asio::post)──▶ runtime(写设备/重配/采样)
```

---

## 5. 数据模型(SQLite + Drogon ORM)

> 配置类表用"规范列 + kind 专属参数 JSON 列"折中:既能 SQL 查询/索引,又能容纳各协议异构参数。

| 表 | 关键列 | 说明 |
|---|---|---|
| `transports` | id(pk text), name, kind(enum), enabled, params_json, scheduler_json, created_at, updated_at | 每个协议端点。`params_json` 放 kind 专属(host/port/slave_id 或 port_name/baud 或 endpoint_url 或 broker_uri…) |
| `datapoints` | id(pk text), transport_id(fk), table(HR/IR/…), addr, type(U16/U32/EnumU16/…), word_order, scale, codec_id(fk null), kind(Status/…), enabled | 采集点定义 |
| `codecs` | id(pk text), kind(enum_u16/lua), params_json, script_path | 值变换 |
| `poll_ranges` | id(pk), transport_id(fk), table, start, count, period_ms, enabled | 轮询窗口 |
| `conversion_rules` | id(pk), name, enabled, source_json, dest_json, transform_json, trigger(onChange/periodic), period_ms | 协议转换规则(§8) |
| `config_versions` | version(pk int auto), status(draft/active/superseded), snapshot_json, author, note, created_at, applied_at | 配置快照 + 热更新版本/回滚 |
| `samples` | id(pk), dp_id(fk), ts(index), value_num, value_text, quality | datapoint 历史(REST 历史查询/图表),带保留策略 |
| `events` | id(pk), ts(index), level(info/warn/error), source, code, message, detail_json | 系统/审计日志(连接状态、apply、转换错误…) |
| `settings` | key(pk), value_json | 全局设置(log_level、**sample_retention_days=30**、cleanup_cron、ws_heartbeat_ms、sample_interval…) |
| `users` | id(pk), username(uniq), password_hash(argon2/bcrypt), role(fk), enabled, created_at, last_login_at | 账号(D5) |
| `roles` | id(pk), name(viewer/operator/admin), description | 角色 |
| `role_permissions` | role_id(fk), permission(text) | 角色→权限位(见 §12);多对多 |
| `api_tokens` | token(pk hash), user_id(fk), name, scopes, expires_at, last_used_at | 机器对机器调用(可选) |
| `audit_log` | id(pk), ts, user_id, action, target, detail_json | 写类操作审计(登录/配置改动/Apply/控制写) |

**索引**:`samples(dp_id, ts)`、`events(ts)`、`datapoints(transport_id)`、`poll_ranges(transport_id)`。
**生成**:`drogon_ctl create model`(由 `db/schema.sql` + `model.json` 生成 Model 类)。

---

## 6. 配置模型与热更新(热生效)

**三态配置 + 显式 Apply。**

- **Draft(草稿)**:前端所有增删改写入"草稿集"(配置类表的当前行即草稿;或 `config_versions` 里 status=draft 的快照)。改动**不立即影响运行时**。
- **Active(生效)**:运行时正在运行的版本(`config_versions.status=active`)。
- **Apply(使其生效)**:前端"应用更改"按钮 → `POST /api/v1/system/config/apply`:
  1. **校验**:Schema/引用完整性(datapoint 指向的 transport/codec 存在、地址范围合法、转换源/目标可达)。可单独 `POST .../validate`(dry-run)。
  2. **快照**:把当前草稿冻结成新 `config_versions`(snapshot_json)。
  3. **下发**:`asio::post(runtimeIo, applyCmd)`,运行时执行 **ConfigApplier**:
     - **Diff** 旧/新 schema → 仅重建变化的 transport/poll/bridge/conversion;未变化的连接**不断开**(零抖动)。
     - v1 可先实现"整体原子重建"(短暂 blip),v2 做增量 diff-apply。
  4. **标记**:新版本 active、旧版本 superseded;失败则保持旧 active 并回报错误。
  5. **回报**:HTTP 响应 + 通过 WS `event` 广播 apply 结果。
- **Rollback**:`POST /api/v1/system/config/versions/{version}/rollback` → 以历史快照重新 Apply。

**前端 UI 入口(D2 要求)**:`Config & Apply` 页显示 **草稿 vs 生效 的 diff**,提供
`Validate` 与 `Apply changes` 两个按钮,以及版本历史 + 回滚。未应用时全局顶栏显示
"● N 项未生效"提示。

> 扩展(P2):插件 DLL 热替换——FieldRuntimeBase 有 plugin 抽象,可在 ConfigApplier 内
> 卸载/加载 plugin so/dll。本期不做。

---

### 6.1 历史数据保留与清理(D10)

- **保留窗口**:默认 `sample_retention_days = 30`(可在系统设置改)。
- **定期清理**:后台调度(drogon 定时器或运行时计时器)按 `cleanup_cron`(默认每日 03:00)执行
  `DELETE FROM samples WHERE ts < now - retention_days`,随后 `PRAGMA incremental_vacuum`(或周期 `VACUUM`)回收空间。
- **手动**:`POST /api/v1/system/maintenance/cleanup`(Admin)立即清理;`GET .../storage` 看库大小与最早样本。
- **可选降采样(P2)**:超过 N 天的样本降为每分钟 avg/min/max,延长可视窗口、压体积。本期先做硬删除。
- **写入端**:历史采样 sink 批量写(事务 + 预编译语句),避免高频单条写拖慢 SQLite。

---

## 7. 协议转换引擎(ConversionEngine)

运行时线程内的规则引擎,**泛化现有 bridge**(bridge 仅 Modbus 镜像;conversion 是任意 dp→dp 跨协议)。

**规则 schema(`conversion_rules`)**
```jsonc
{
  "id": "modbus_temp_to_mqtt",
  "name": "温度上云",
  "enabled": true,
  "source": { "transport": "plc1", "table": "HR", "addr": 0, "codec": "scale_0_1" },
  "dest":   { "transport": "mqtt1", "topic": "field/temp" },     // 或 Modbus: {transport,table,addr}
  "transform": { "kind": "scale", "scale": 0.1 },                // none|scale|formula|lua
  "trigger": "onChange",                                         // onChange|periodic
  "period_ms": 0
}
```

**执行**
- 启动时 ConversionEngine 订阅各规则 `source` 的 datapoint 变化(EventBus)。
- 源变化 → 取值 → `transform`(none/scale/公式/lua)→ 写 `dest`:
  - dest=Modbus → `transport.writeAsync`(HR);dest=MQTT → publish;dest=OPC UA → write node。
- 错误/限流计数 → SnapshotStore(`conversion/<id>` 统计) + events 表。

**典型转换**:Modbus→MQTT(寄存器上云)、MQTT→Modbus(云下发写寄存器)、
OPC UA→Modbus/MQTT、Modbus TCP↔RTU 网关桥接。

**前端**:`Conversion` 页 —— 规则列表(启停)、源/目标双侧选择器(联动可用 transport 与点位)、
变换编辑、**实时活动**(每条规则的命中速率/最近值/错误,经 WS `conversion/<id>` 流)。

---

## 7.5 数据仿真服务(Data Service / Mock,用于显示验证)

> D8。**目的**:无需真实硬件,即可让 `Data Service → FieldRuntimeBase → drogon → React`
> 整链跑通、看到"会动的数据"。**完整数据字典(类型 + 变化规律 + 点目录)见
> [`DATA_SERVICE.md`](./DATA_SERVICE.md)**,本节只讲它在架构中的位置与接口面。

**形态**:一个独立可执行 `field_console_dataservice`(无 Qt),在现有 mock
(`MockModbusTcpServer` / `OpcUaMockServer` / `MqttMockBroker`/RTU mock)之上统一为
**可配置的数据仿真**,对外同时暴露三种南向面,供运行时当作"真设备"接入:

| 面 | 角色 | 运行时如何接 |
|---|---|---|
| Modbus TCP server | 持有 HR/IR 寄存器,按规律刷新 | `modbus_tcp_client` 轮询 |
| OPC UA server | 持有 `ns=2;s=Sim_*` 节点 | `opc_ua_client` 读 |
| MQTT publisher | 向 broker 发 `sim/<id>` | `mqtt_client` 订阅(或内置 mock broker) |
| S7 server(snap7) | 持有 DB 区(如 DB1) | `s7_client` 读 `DBx.DB*`(S7 本期新增) |

**仿真模型**(详见 DATA_SERVICE.md):每个**仿真点**= {id, 协议面, 地址/节点/topic,
**数据类型**(bool/u16/i16/u32/i32/f32/enum + 字节序), **变化规律 pattern**
(constant/counter/sine/triangle/sawtooth/square/random_walk/uniform_random/
step_sequence(enum 轮转)+ 可叠加高斯噪声), 参数(min/max/amplitude/offset/
period_ms/step/seed), **刷新周期** update_period_ms}。

**配置**:`sim.toml`(或 console 通过专门接口下发);提供一套**默认点目录**,覆盖三协议 ×
各数据类型 × 各 pattern,与 console 默认配置一一对应——一键起 sim + console 即见动态数据。

**异常注入(可选,验证质量与重连)**:按点/全局注入 断连 / 读超时 / 坏值(NaN/越界)/
冻结,用于验证 datapoint `quality`、transport 重连、conversion 错误计数。

**与 console 的关系**:Data Service 是**独立进程**,不属于 console 后端;console 只是把它当
普通设备配置进来。生产环境用真实设备替换它,console/运行时零改动。

---

## 8. REST API 设计

**约定**:前缀 `/api/v1`;JSON;统一响应包
`{ "code":0, "message":"ok", "data":... }`(非 0 为错误码);分页
`?page=0&size=50`(page 0-indexed,后端 `max(0,page)` 钳位,沿用项目约定);
鉴权 `Authorization: Bearer <token>`(D5,可关)。

### 8.1 分类总览(用于文档/Swagger tag)

| 分组 | tag | 前缀 | 职责 |
|---|---|---|---|
| A 系统/配置 | `system` | `/system` `/config` | 健康、设置、配置版本、apply/rollback、日志 |
| B 协议配置 | `transports` | `/transports` | 协议端点 CRUD、测试连接、协议元数据(参数 schema) |
| C 采集点配置 | `datapoints` | `/datapoints` `/codecs` `/polls` | 点位/编解码/轮询 CRUD |
| D 协议转换 | `conversions` | `/conversions` | 转换规则 CRUD、启停、测试 |
| E 数据 | `data` | `/data` | 最新值、历史、单点/批量读、写(控制) |
| F 实时 | `ws` | `/ws/*` | WebSocket(见 §9) |
| G 文档 | `docs` | `/api/docs` | Swagger UI + openapi.yaml |
| H 鉴权/用户 | `auth` | `/auth` `/users` `/roles` | 登录、用户与角色权限管理(见 §12) |

### 8.2 端点清单(节选,完整见 `openapi.yaml`)

**A 系统/配置**
```
GET    /system/health                      存活/版本/运行时状态摘要
GET    /system/info                        构建信息、已编译协议、能力开关
GET    /system/settings                    全局设置
PUT    /system/settings                    更新设置(部分热生效)
GET    /system/events?level=&page=&size=   事件/审计日志(分页)
GET    /config/draft                       当前草稿全量
GET    /config/active                      当前生效全量
GET    /config/diff                        草稿 vs 生效 差异(驱动 Apply 页)
POST   /config/validate                    校验草稿(dry-run)→ 错误列表
POST   /config/apply                       应用草稿(热生效)→ 新 version
GET    /config/versions?page=&size=        版本历史
POST   /config/versions/{v}/rollback       回滚到某版本
```

**B 协议配置**
```
GET    /transports/kinds                   支持的协议种类 + 各自参数 JSON Schema(驱动动态表单)
GET    /transports?kind=&enabled=          列表
POST   /transports                         新建(写草稿)
GET    /transports/{id}                    详情
PUT    /transports/{id}                    修改(写草稿)
DELETE /transports/{id}                    删除(写草稿)
POST   /transports/{id}/test               测试连接(临时连一次,不入生效集)
GET    /transports/{id}/state              当前连接状态(来自 SnapshotStore)
```

**C 采集点/编解码/轮询**
```
GET|POST|PUT|DELETE  /datapoints[/{id}]    点位 CRUD
GET|POST|PUT|DELETE  /codecs[/{id}]        编解码 CRUD
GET|POST|PUT|DELETE  /polls[/{id}]         轮询窗口 CRUD
```

**D 协议转换**
```
GET|POST|PUT|DELETE  /conversions[/{id}]   规则 CRUD
POST   /conversions/{id}/enable|disable    启停
POST   /conversions/{id}/test              用样例值试跑变换(不写设备)
GET    /conversions/{id}/stats             命中速率/最近值/错误计数
```

**E 数据(主动拉取)**
```
GET    /data/latest?ids=a,b,c              批量最新值(SnapshotStore 快照)
GET    /data/points/{id}                   单点最新值
GET    /data/history?id=&from=&to=&page=&size=&agg=   历史(可聚合 avg/min/max/raw)
POST   /data/read                          强制立即采样 {transport,table,addr,count} → 直读设备
POST   /data/write                         控制写 {transport,table,addr,values}(需鉴权)
```

**H 鉴权 / 用户**
```
POST   /auth/login                         {username,password} → {accessToken, refreshToken, user{role,permissions}}
POST   /auth/refresh                       刷新 access token
POST   /auth/logout                        失效 refresh token
GET    /auth/me                            当前用户 + 权限位
PUT    /auth/me/password                   改自己密码
GET|POST|PUT|DELETE  /users[/{id}]         用户管理(Admin)
GET    /roles                              角色与其权限位
PUT    /roles/{id}/permissions             调整角色权限(Admin)
GET|POST|DELETE      /api-tokens[/{id}]    机器令牌(Admin/自助)
```

**统一错误码**(节选):`1xxx` 参数、`2xxx` 鉴权/权限(401/403)、`3xxx` 配置校验、`4xxx` 运行时/设备、`5xxx` 内部。

---

## 9. WebSocket API 设计

**端点** `/ws/stream`(可带 `?token=`)。**消息均为 JSON**。

**客户端 → 服务端**
```jsonc
{ "op": "subscribe",   "topics": ["dp/plc1.temp", "dp/*", "transport/plc1", "events", "conversion/r1"] }
{ "op": "unsubscribe", "topics": ["dp/*"] }
{ "op": "ping" }
```
topic 形态:`dp/<id>` 或 `dp/*`(全部点)、`transport/<id>`(连接状态)、`events`、`conversion/<id>`。

**服务端 → 客户端**
```jsonc
{ "type":"dp",        "id":"plc1.temp", "value":23.0, "raw":230, "ts":1718700000123, "quality":"good" }
{ "type":"transport", "id":"plc1", "state":"connected" }
{ "type":"event",     "level":"warn", "code":4001, "message":"plc1 read timeout", "ts":... }
{ "type":"conversion","id":"r1", "hit":1842, "lastValue":23.0, "errors":0, "ts":... }
{ "type":"pong" }
{ "type":"ack",       "op":"subscribe", "topics":[...] }
```

**策略**:每订阅连接维护 topic 集合;推送在连接所属 trantor loop 内 `send`;**慢客户端按
dp 做 latest-wins 合并**(只保最新值,丢中间帧)防积压;`ws_heartbeat_ms` 心跳;断线前端自动重订阅。

---

## 10. API 文档方案

- **`docs/openapi.yaml`**(OpenAPI 3.0,手维护;按 §8.1 的 tag 分组)为单一事实源。
- **Swagger UI** 静态站点挂 `/api/docs`(drogon 静态服务),读 `openapi.yaml`。
- **`docs/API.md`**:人读版(分类、鉴权、错误码、典型流程:配置→Apply→订阅)。
- 端点变更时三者同步(可后续加 CI 校验 openapi 与控制器路由一致)。

---

## 11. 前端架构(React)+ 页面清单(供 Figma)

**栈**:React + TypeScript + Vite;Ant Design(表格/表单/抽屉)+ React Query(服务端态)+
Zustand(UI 态)+ ECharts(趋势)+ 自研 `useWsStream` hook(订阅/重连/latest-wins)。
**亮点**:协议表单**由后端 `/transports/kinds` 的 JSON Schema 动态渲染**(加协议=后端加一种 kind,前端零改)。

**页面 ↔ 后端契约映射(给 Figma 设计时对齐)**

| # | 页面 | 主要数据/动作 | 依赖接口 |
|---|---|---|---|
| 1 | Dashboard 概览 | 连接状态卡、吞吐、最近事件(实时) | `GET /system/health`、WS `transport/*` `events` |
| 2 | Protocols 协议 | transport 列表 + 动态表单 + 测试连接 | B 组全部 + `GET /transports/kinds` |
| 3 | Datapoints 采集点 | 点位/编解码表格与表单 | C 组 |
| 4 | Polling 轮询 | 轮询窗口配置 | `/polls` |
| 5 | Conversion 协议转换 | 规则列表、源/目标联动选择、变换、实时活动 | D 组 + WS `conversion/*` |
| 6 | Live 实时监控 | 点位实时表/图、过滤、"立即拉取" | WS `dp/*` + `GET /data/latest` + `POST /data/read` |
| 7 | History 历史 | 时间范围查询、表+图、导出 | `GET /data/history` |
| 8 | **Config & Apply** | 草稿 vs 生效 diff、Validate、**Apply**、版本/回滚 | A 组 config/* |
| 9 | Settings 系统设置 | 日志级别、保留期、清理、WS | `/system/settings`、`/system/maintenance/*` |
| 10 | API Docs | 内嵌 Swagger | `/api/docs` |
| 11 | Logs 事件日志 | 分页事件 + 审计日志 | `GET /system/events`、`/audit` |
| 12 | **Users & Roles** | 用户增删、角色权限分配(Admin) | H 组 `/users` `/roles` |
| 0 | **Login** | 账号密码登录 | `POST /auth/login` |

**角色工作区(D5)**:登录后按角色加载导航与默认落地页 —— **Viewer** 只见 1/6/7(监控工作区),
**Operator** 增 5 与控制写,**Admin** 见全部。路由守卫 + 菜单过滤 + 按钮级 disable 都由 `/auth/me`
返回的 permission 位驱动(后端是最终权威,前端只做体验)。
**全局组件**:顶栏"未生效项"徽标(链到页 8,仅有 config 权限者可见)、连接/ WS 状态灯、当前用户与角色。
**开发期**:前端 Vite dev server + 后端开 CORS;**生产**:`npm run build` 产物由 drogon 静态托管(单进程)。

---

## 12. 鉴权与权限(Auth + RBAC,D5)

**单套前端 + RBAC + 角色工作区**(评估结论):一套代码,按角色裁剪可见性,而非两套独立前端。
需要物理隔离(监控大屏 vs 配置台)时,用反代路径白名单或构建开关达成,**不分叉代码库**。

### 12.1 鉴权机制
- **JWT**:`POST /auth/login` 校验 `argon2`/`bcrypt` 口令 → 签发 **access token**(短期,15 min,
  含 `sub/role/permissions`)+ **refresh token**(长期,DB 可吊销)。`/auth/refresh` 续期。
- **AuthFilter**(drogon Filter)校验 `Authorization: Bearer`;白名单:`/auth/login`、`/system/health`、
  `/api/docs`、前端静态资源。**WS** 握手校验 `?token=`(或子协议头)。
- **机器令牌** `api_tokens`(带 scopes/过期)给脚本/集成用;同一鉴权管线。
- TLS 由前置反向代理(nginx/caddy)终结。

### 12.2 角色与权限位(RBAC)
权限是**细粒度位**,角色是其捆绑(`role_permissions` 可由 Admin 调整):

| permission | 含义 | Viewer | Operator | Admin |
|---|---|:--:|:--:|:--:|
| `data:read` | 看实时/历史/快照 | ✓ | ✓ | ✓ |
| `data:write` | 控制写 `/data/write` | | ✓ | ✓ |
| `config:read` | 看协议/点位/转换配置 | | ✓ | ✓ |
| `config:write` | 改草稿(CRUD) | | ✓ | ✓ |
| `config:apply` | Apply/回滚(热生效) | | | ✓ |
| `conversion:manage` | 转换规则增改启停 | | ✓ | ✓ |
| `system:settings` | 系统设置/清理 | | | ✓ |
| `user:manage` | 用户与角色权限 | | | ✓ |
| `sim:control` | 仿真控制(仅 dev 模式) | | ✓ | ✓ |

> 你点名的两类权限 = **`config:*`(配置系统)** 与 **`data:read`(观察数据)**;
> Viewer=纯观察,Admin=配置系统全权。中间的 Operator 可按需裁。

### 12.3 落地
- **后端是权威**:每个路由声明所需 permission,`AuthFilter`/控制器统一校验;前端隐藏只是体验。
- **审计**:登录、配置改动、Apply、控制写、用户变更写 `audit_log`(§5)。
- **初始化**:首次启动建默认 `admin`(强制改密)+ 内置三角色;支持环境变量注入初始口令。

---

## 13. 构建 / 依赖 / 部署

- **后端依赖**:`drogon`(vcpkg,拉 trantor/jsoncpp)、`sqlite3`(已在用)、`FieldRuntime::Base`、
  asio transports、**JWT 库**(jwt-cpp,header-only)、**口令哈希**(libsodium/argon2)、
  **`snap7`**(S7 通信,LGPL —— vcpkg 可能无,需手动接入,**选型+许可证待确认**)。
  **结构改动**:把 `gateway/Asio*Client/Server`(目前编进 `field_gateway` exe)抽成
  `field_gateway_transports` 静态库,供 `field_gateway` 与 `web_console` 共用;新增 `AsioS7Client` 也入此库。
- **CMake**:`option(BUILD_WEB_CONSOLE "..." OFF)`;开启时 `find_package(Drogon CONFIG REQUIRED)`。
- **前端**:`frontend/`(独立 `package.json`/Vite);CMake 可选 `WEB_CONSOLE_BUILD_FRONTEND` 调 `npm build` 并 install 到 `share/web_console/www`。
- **部署**:单二进制 `web_console` + `console.db`(SQLite)+ `www/`(前端);配置经 UI 持久化进 DB,无需 toml(可保留 `--import toml` 做迁移)。

---

## 14. 目录结构(规划)

```
core/examples/web_console/
├── CMakeLists.txt
├── docs/
│   ├── ARCHITECTURE.md        ← 本文
│   ├── DATA_SERVICE.md        ← 数据仿真:数据类型 + 变化规律 + 点目录(D8)
│   ├── API.md                 ← 人读 API 参考(待写)
│   └── openapi.yaml           ← OpenAPI 3.0(待写)
├── dataservice/               ← 数据仿真服务(独立 exe field_console_dataservice)
│   ├── main.cpp
│   ├── SimEngine.*            ← pattern 求值 + 刷新调度
│   ├── ModbusSimServer.* / OpcUaSimServer.* / MqttSimPublisher.* / S7SimServer.*
│   └── sim.toml               ← 默认仿真点目录
├── backend/
│   ├── main.cpp               ← drogon app + RuntimeHost 启停
│   ├── controllers/           ← System/Transport/Datapoint/Conversion/Data/Auth/User 控制器
│   ├── filters/               ← AuthFilter(JWT)/ CorsFilter
│   ├── ws/                    ← StreamWsController + WsHub
│   ├── services/              ← Config/Transport/Datapoint/Conversion/DataQuery/Auth/User Service
│   ├── models/                ← drogon ORM 生成的 Model + DAO
│   ├── runtime/               ← RuntimeHost / SnapshotStore / RuntimeBridge / ConfigApplier / ConversionEngine
│   ├── db/                    ← schema.sql + migrations/
│   └── config/                ← drogon model.json / app 配置
└── frontend/                  ← React(Figma 设计后填充)
    ├── package.json
    └── src/{pages,components,api,ws,store}
```

---

## 15. 实施路线图(后端,分阶段)

| 阶段 | 内容 | 产出/验证 |
|---|---|---|
| W0 数据仿真 | `field_console_dataservice`:SimEngine + 三协议面 + `sim.toml` 默认点目录 + DATA_SERVICE.md | 起 sim,手动用 modbus/opc/mqtt 客户端读到按规律变化的值 |
| W1 骨架 | drogon app + SQLite + ORM 模型 + `/system/health` + 静态托管 | 起得来、能读写一张表 |
| W2 配置 CRUD | B/C 组接口 + `transports/kinds` schema + 草稿持久化 | Postman 跑通 CRUD |
| W3 运行时桥 | 抽 transports 静态库 + RuntimeHost 从 DB 构建装配 + SnapshotStore | 真连一个 mock,latest 有值 |
| W4 数据接口 | E 组(latest/history/read/write)+ 历史采样 sink | REST 拉到实时与历史 |
| W5 WebSocket | WsHub + 订阅协议 + RuntimeBridge 推送 + latest-wins | 浏览器订阅看实时流 |
| W6 热生效 | ConfigService(validate/apply/版本/回滚)+ ConfigApplier 优雅重配 + 历史保留清理 | 改配置→Apply→不重启生效 |
| W7 协议转换 | ConversionEngine + D 组接口 + 实时活动 | Modbus→MQTT 端到端 |
| **W7.5 S7 南向** | **运行时 `AsioS7Client`(snap7)+ 数据仿真 `S7SimServer`** | S7 DB 读到仿真值(与 OPC UA/RTU 同量级新实现) |
| **W7.6 鉴权 RBAC** | users/roles/permissions + JWT + AuthFilter + 用户管理接口/页;前端路由守卫 | 三角色登录,权限正确放行/拦截 |
| W8 文档/打磨 | openapi.yaml + Swagger UI + API.md + 前端联调 | Swagger 可点、前端跑通 |

> 复用红线沿用 gateway:无 Qt;`dumpbin/ldd` 仅 drogon/sqlite/snap7/jwt/系统;每阶段可加 e2e。
> **S7(W7.5)是本期唯一从零的南向**,可与 W1–W7 并行先行验证(snap7 选型 + 许可证先确认)。

---

## 16. 决策状态

- ✅ 全部确认:D1 复用 FieldRuntimeBase · D2 配置热生效 · D8 独立数据仿真 ·
  D9 协议=Modbus TCP/OPC UA/MQTT/S7(RTU 不纳入)· D10 历史 30 天+定期清理 ·
  D5 强制鉴权+RBAC(单套前端+角色工作区)· D7 放 `core/examples/web_console` 作 core 完整示例。
- ✅ **S7 已落地**:选 snap7(vcpkg `snap7` 1.4.2,LGPL,动态库)。网关侧 `AsioS7Client`
  + `field_gateway_s7_mock_server` 已实现并 Windows e2e 过(读 DB1 → 23.0/1450/fault)。
  数据仿真 S7 面复用此 mock。(注:snap7 走 SourceForge 下载,国内首装较慢。)
