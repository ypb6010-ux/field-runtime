# FieldRuntime Web Console Frontend

React 18 + TypeScript + Vite 的工业网关管理界面。页面只使用真实 REST/WebSocket 数据，不内置
演示账号或随机业务数据。

## 开发

```powershell
npm ci
npm run dev
```

开发服务器默认将 `/api` 和 `/ws` 代理到 `127.0.0.1:8080`，可在 `vite.config.ts` 修改。

## 生产构建

```powershell
npm ci
npm run build
```

产物位于 `dist`，可作为 `web_console_backend` 的第三个参数由 Drogon 直接托管。

登录 token 仅存于当前标签页的 `sessionStorage`。首次管理员密码由后端
`FIELD_CONSOLE_ADMIN_PASSWORD` 或启动日志决定，详见上级 [`README.md`](../README.md)。

界面组件基于 shadcn/ui 与 Radix primitives，许可说明见 [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md)。
