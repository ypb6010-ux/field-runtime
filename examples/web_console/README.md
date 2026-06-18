# web_console — FieldRuntime 网关 Web 管理控制台

core 的**完整功能示例**:Drogon(C++)后端 + SQLite + React(待 Figma)前端,**内嵌
FieldRuntime 运行时**(复用 gateway 的 transports/assembly),做协议配置、实时订阅、历史、
配置热生效、协议转换、鉴权 RBAC。无 Qt。

设计见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) 与 [`docs/DATA_SERVICE.md`](docs/DATA_SERVICE.md)。

## 组件

| 可执行 | 作用 |
|---|---|
| `field_console_dataservice` | 数据仿真:Modbus(:1502)/ S7(:102)/ OPC UA(:4840)三面,SimEngine 按规律产出动态数据 |
| `web_console_backend` | Drogon 后端:REST + WebSocket + SQLite + 内嵌 RuntimeHost |

## 构建

随 `CORE_BUILD_GATEWAY` 一起构建(无 Qt):

```
cmake -S core -B build -G Ninja -DCORE_WITH_QT=OFF -DCORE_BUILD_GATEWAY=ON \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --target field_console_dataservice web_console_backend
```

依赖(vcpkg):`drogon[sqlite3]`、`open62541`、`snap7`、`asio`、`sqlite3`。

## 运行

```
# 1) 数据仿真(三协议面)
field_console_dataservice 1502 127.0.0.1 4840

# 2) 后端(默认 runtime.toml 连上面三面)
web_console_backend console.db 8080

# 3) 浏览器
http://127.0.0.1:8080/            前端占位页(加载 health)
http://127.0.0.1:8080/api/docs    Swagger UI(OpenAPI)
```

默认账号:`admin/admin`(全权)、`viewer/viewer`(只读)。

## 演示流程

1. `POST /api/v1/auth/login` 拿 token。
2. `POST /api/v1/transports`、`/datapoints`、`/poll_ranges` 配置(指向数据服务)。
3. `POST /api/v1/config/apply` 热生效 → 运行时不重启切到新配置。
4. `GET /api/v1/data/latest` 看实时值;`ws://…/ws/stream` 订阅实时流;`GET /api/v1/data/history?id=…` 看历史。
5. `POST /api/v1/conversions` 建协议转换规则;`GET …/stats` 看命中。

## 阶段(W0–W8,均已实现+验证)

W0 数据仿真 · W1 Drogon 骨架 · W2 配置 CRUD · W3 内嵌 RuntimeHost · W4 数据 API/采样 ·
W5 WebSocket · W6 配置热生效 · W7 协议转换 · W7.6 鉴权 RBAC · W8 OpenAPI/Swagger。

## 已知限制

- 前端为占位页,正式 React UI 待 Figma 设计后填充(后端契约见 `docs/ARCHITECTURE.md` §11 + `/api/docs`)。
- 鉴权用 opaque token + MD5(demo);生产换 JWT + argon2。
- 协议转换 dest 当前走 Modbus(MQTT dest 需 broker)。
- 口令/会话为内存态;多实例部署需外置。
