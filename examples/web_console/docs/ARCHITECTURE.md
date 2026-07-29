# Web Console 当前架构

本文描述当前代码的运行路径，不是未来规划。接口细节以
[`openapi.yaml`](openapi.yaml) 和后端控制器注册为准。

## 1. 组件与线程边界

```text
Browser
  ├─ React SPA
  ├─ REST /api/v1/*
  └─ WebSocket /ws/stream
          │
          ▼
Drogon / trantor 线程
  ├─ Auth + RBAC
  ├─ 配置、数据、转换、管理控制器
  ├─ SQLite
  ├─ 历史采样与 WebSocket 推送
  └─ RuntimeHost 快照
          │ 受控跨线程调用
          ▼
独立 asio io_context 线程
  └─ GatewayAssembly
       ├─ Modbus TCP client
       ├─ OPC UA client
       └─ S7 client
```

`RuntimeHost` 是 Web 线程与 FieldRuntime asio 线程之间的唯一桥。运行时对象只在 asio
线程内启动、停止、重载和写入；REST/WebSocket 只读取互斥保护的快照。析构时先停止快照定时器和
运行时，再终止 io 线程，避免后台回调访问已销毁对象。

## 2. 配置模型

SQLite 中的下列表是“草稿配置”：

- `transports`
- `codecs`
- `datapoints`
- `poll_ranges`

控制台当前只暴露 `modbus_tcp_client`、`opc_ua_client`、`s7_client`。Transport 参数表单由
`GET /api/v1/transports/kinds` 返回的字段描述生成。后端仍会独立执行类型、范围、未知字段、
地址空间和引用完整性检查，不能依赖前端校验。

删除被 conversion rule 引用的 transport/datapoint，或删除仍被 datapoint 引用的 codec，
会返回 `409 Conflict`。Transport 删除在确认无转换引用后，由数据库外键级联删除其 datapoint
和 poll range。

## 3. 发布与回滚

```text
编辑 SQLite 草稿
  → /config/status 比较当前草稿和 active snapshot
  → /config/validate 生成临时 TOML 并进行丢弃式装配
  → /config/apply
       1. 再次生成并校验候选配置
       2. RuntimeHost 事务式 reload
       3. 成功后记录 active config_version
  → /config/versions/{version}/rollback 复用同一校验/重载路径
```

候选装配失败不会停止当前运行图。候选启动异常时会恢复旧装配。配置操作使用 try-lock，
同一时刻只允许一个，重入请求直接返回 409。成功生成的
`<database-path>.runtime.toml` 会在下次进程启动时优先加载，因此发布结果跨重启保持。

## 4. 数据路径

- `RuntimeHost` 周期性产生 datapoint/transport 快照。
- `/data/latest`、`/data/points/{id}` 和 `/runtime/transports` 读取运行快照。
- `/data/catalog` 只返回监控所需的非敏感草稿元数据，Viewer 可读取。
- 历史采样每 2 秒检查一次，只写入时间戳变新的点位。一次最多 150 行组成一个 SQLite
  multi-row insert；失败批次会在后续周期重试。
- 保留任务每小时执行一次；`sample_retention_days` 可在 Settings 修改，也可手动立即清理。
- WebSocket 每秒构造一次快照，按订阅 topic 为每个连接过滤；发送发生在连接集合锁之外。
- 浏览器只为选中的最多 5 个数值点保留趋势数据，每点最多 3600 个样本。

## 5. Conversion Engine

规则保存在 `conversion_rules`，不属于运行时 TOML：

1. 每秒或规则变更后刷新规则缓存；
2. 每 200 ms 获取一次 datapoint 快照，并按 id 建索引；
3. `onChange` 只在源值变化时执行，失败后至少等待 5 秒再试；
4. `periodic` 按 `period_ms` 执行；
5. 比例变换并检查 0..65535 输出边界后，写入目标 transport 的 holding register；
6. 每条规则同一时刻最多一个 in-flight 写入。

`/conversions/stats` 提供批量统计，避免前端按规则产生 N+1 请求；单规则统计接口保留用于诊断。

## 6. 鉴权与权限

- 首次启动空数据库时创建 `admin`。初始密码来自
  `FIELD_CONSOLE_ADMIN_PASSWORD`，未设置则随机生成并只打印到标准错误。
- 新口令使用 PBKDF2-HMAC-SHA256（随机 salt，210000 次）。旧 MD5 hash 仅用于登录时验证，
  成功后立即升级。
- opaque token 由 OpenSSL CSPRNG 生成，服务端内存保存 8 小时；每用户最多 20 个、全局最多
  4096 个 session。
- 登录按用户名和源 IP 做 15 分钟窗口限流，并对不存在的用户执行 dummy PBKDF2。
- token 只存于浏览器 `sessionStorage`，关闭标签页会丢失。
- WebSocket 每次推送前重新校验 session；注销、改密或会话过期会关闭已有连接。
- Viewer：监控只读；Operator：监控、控制写、配置编辑、转换管理；Admin：再增加配置发布、
  系统设置和用户管理。
- 所有受保护路由在后端校验 permission。前端导航和按钮隐藏只用于改善体验。
- 写请求成功后异步记录 audit；响应设置 CSP、frame、referrer、nosniff 和 no-store 头。

## 7. 稳定性和资源边界

- HTTP body 上限 1 MiB；分页、历史范围、写寄存器数量、WebSocket 连接、消息和 topic 数均有上限。
- 连接测试使用异步 TCP probe，最大并发 64，超出返回 429。
- WebSocket 快照发送和 Conversion 写入避免在全局互斥锁内调用外部对象。
- 配置文件写入采用临时文件，运行时生命周期操作串行化。
- SQLite schema 使用外键、CHECK 和索引维持基础约束；应用层处理跨 JSON 字段的引用关系。

## 8. 部署边界

开发时 Vite 将 `/api` 和 `/ws` 代理到 `127.0.0.1:8080`。生产时后端可直接托管 `dist`。
建议由反向代理提供 TLS、访问日志脱敏、请求速率限制和可信网络边界。

当前限制：

- session 和登录限流是单进程内存状态，多实例需要外置；
- WebSocket 浏览器握手通过查询参数传 token，应避免记录完整查询字符串；
- Swagger UI 从 unpkg 加载资源，离线现场应改成本地静态资源；
- 控制台的 conversion destination 当前只支持 Modbus holding register 写入；
- 历史数据没有降采样层，大规模长期留存应接入时序数据库或分层归档。

## 9. 验证建议

构建成功不等于功能验收。至少验证：

1. 空数据库首次启动和随机密码；
2. Viewer/Operator/Admin 的 REST、导航和按钮权限；
3. 配置校验、发布失败保持旧运行图、版本回滚；
4. 三种 transport 的连接、断线、重连和停机；
5. WebSocket 重连、慢客户端和大消息；
6. 历史采样失败重试、自动保留和立即清理；
7. conversion 的 onChange、periodic、限幅、超时和规则删除。
