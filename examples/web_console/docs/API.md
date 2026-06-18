# API 参考

权威机器可读规范见 [`openapi.yaml`](./openapi.yaml),运行时由后端在 **`/api/docs`**(Swagger UI)
提供交互式浏览。

**约定**:统一响应包 `{ "code":0, "message":"ok", "data":... }`;鉴权
`Authorization: Bearer <token>`(POST `/api/v1/auth/login` 获取);分页 `?page=0&size=...`。

**分组(tag)**:

| 分组 | 前缀 | 说明 |
|---|---|---|
| system | `/system` | 健康、构建信息、事件日志 |
| auth | `/auth` | 登录 / me / logout(RBAC:viewer 只读,operator/admin 可写) |
| transports | `/transports` | 协议端点 CRUD + `/transports/kinds` 参数 schema |
| datapoints | `/datapoints` `/codecs` `/poll_ranges` | 采集点 / 编解码 / 轮询 CRUD |
| conversions | `/conversions` | 协议转换规则 CRUD + enable/disable + stats |
| data | `/data` `/runtime` | latest / history / write / 运行时连接状态 |
| config | `/config` | validate / apply(热生效)/ versions / rollback |

**WebSocket**:`/ws/stream` —— 发送 `{op:"subscribe",topics:["dp/*","transport/*"]}` / `{op:"ping"}`;
服务端每秒推 `{type:"snapshot",datapoints:[...],transports:[...]}`。
