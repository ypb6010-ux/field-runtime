# API 参考

权威机器可读规范见 [`openapi.yaml`](openapi.yaml)，运行时由后端在 `/api/docs`
提供 Swagger UI。

## 通用约定

- REST base path：`/api/v1`
- JSON envelope：`{ "code": 0, "message": "ok", "data": ... }`
- 鉴权：`Authorization: Bearer <token>`
- token 来源：`POST /auth/login`
- 分页：`page` 从 0 开始；各接口会限制 `size` 上限
- 时间：epoch milliseconds
- 请求 body 上限：1 MiB

HTTP 状态码和 envelope code 同时有意义：401/403 表示会话或权限错误，404 表示资源不存在，
409 表示仍有引用，429 表示登录或连接探测限流，500/503 表示服务端或健康状态异常。

## 接口分组

| 分组 | 路径 | 说明 |
|---|---|---|
| system | `/system/health` `/system/info` `/system/events` | 健康、构建信息、事件 |
| auth | `/auth/login` `/auth/me` `/auth/logout` | 登录、当前用户、注销 |
| transports | `/transports` `/transports/kinds` `/transports/{id}/test` | 草稿 transport CRUD、动态表单、连接测试 |
| datapoints | `/datapoints` `/codecs` `/poll_ranges` | 草稿点位、编解码、轮询范围 |
| conversions | `/conversions` `/conversions/stats` | 立即生效的转换规则及统计 |
| data | `/data/catalog` `/data/latest` `/data/points/{id}` `/data/history` `/data/write` | 监控、历史和控制写 |
| control | `/control/config` `/control/runtime` `/control/write` `/control/routes/activate` | 设备控制草稿、运行态、目标字节写和活动路由 |
| runtime | `/runtime/transports` | 当前运行时连接状态 |
| config | `/config/status` `/config/validate` `/config/apply` `/config/versions` | 草稿状态、发布和回滚 |
| administration | `/users` `/roles` `/audit` `/system/settings` `/system/maintenance/*` | 用户、权限、审计、保留策略 |

## 权限

| 权限 | 典型接口 |
|---|---|
| `data:read` | latest、history、runtime state、system info |
| `data:write` | `/data/write` `/control/write` `/control/routes/activate` |
| `config:read` | transport/datapoint/codec/poll/config/control 查询 |
| `config:write` | 草稿 CRUD 和 transport test |
| `config:apply` | apply、rollback |
| `conversion:manage` | conversion CRUD、启停、统计 |
| `system:settings` | settings 和 maintenance |
| `user:manage` | users、roles、audit |

角色是固定的：

- Viewer：`data:read`
- Operator：监控和控制写、配置读写、转换管理
- Admin：全部权限

后端始终执行权限判断；前端导航和按钮状态不构成安全边界。

## WebSocket

路径：`/ws/stream?token=<session-token>`。

浏览器连接后发送：

```json
{"op":"subscribe","topics":["dp/*","transport/*"]}
```

也可使用 `dp/<id>`、`transport/<id>` 精确订阅，或发送 `unsubscribe`。心跳：

```json
{"op":"ping"}
```

服务端返回 `pong`、订阅 `ack`，并每秒推送：

```json
{
  "type": "snapshot",
  "datapoints": [{"id":"sim.temperature","value":25.2,"quality":"good","ts":1785226000000}],
  "transports": [{"id":"plc","kind":"modbus_tcp_client","state":"connected"}]
}
```

单条客户端消息最大 64 KiB，每个连接最多 256 个 topic，服务端最多同时保留 256 个连接。
推送前会重新校验 session；注销、修改密码或 8 小时会话过期后，已有连接也会被关闭。服务端在
连接集合锁外发送，慢客户端由底层连接队列和 latest-wins 页面模型吸收。部署时应对访问日志中的
`token` 查询参数脱敏。
