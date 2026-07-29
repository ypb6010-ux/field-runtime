import { useState, type FormEvent } from "react";
import { Cpu, ShieldCheck, Loader2, AlertCircle, ServerCrash } from "lucide-react";
import type { AuthUser } from "../types";
import { login, AuthError } from "../auth";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Label } from "../components/ui/label";
import { Card } from "../components/ui/card";
import { Alert, AlertTitle, AlertDescription } from "../components/ui/alert";
import { StatusLight } from "../components/StatusLight";

interface LoginProps {
  onAuthenticated: (user: AuthUser) => void;
}

type FormError = {
  code: "empty" | "invalid" | "unreachable" | "server_error" | "rate_limited";
  message: string;
};

export function Login({ onAuthenticated }: LoginProps) {
  const [account, setAccount] = useState("");
  const [password, setPassword] = useState("");
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<FormError | null>(null);
  const [touched, setTouched] = useState(false);

  const accountEmpty = touched && account.trim() === "";
  const passwordEmpty = touched && password === "";

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    setTouched(true);
    setError(null);

    // 5. 账号或密码为空的校验提示
    if (account.trim() === "" || password === "") {
      setError({ code: "empty", message: "请输入账号和密码。" });
      return;
    }

    setSubmitting(true);
    try {
      const user = await login(account, password);
      onAuthenticated(user); // 登录成功 → 由 /auth/me 决定角色与权限
    } catch (err) {
      if (
        err instanceof AuthError
        && (err.code === "unreachable" || err.code === "server_error")
      ) {
        setError({ code: err.code, message: err.message });
      } else if (err instanceof AuthError) {
        setError({
          code: err.code === "rate_limited" ? "rate_limited" : "invalid",
          message: err.message,
        });
      } else {
        setError({ code: "unreachable", message: "发生未知错误，请稍后重试。" });
      }
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <div className="flex h-full w-full items-center justify-center bg-sidebar p-6">
      <Card className="grid w-full max-w-3xl grid-cols-1 overflow-hidden p-0 md:grid-cols-2">
        {/* 品牌侧 */}
        <div className="hidden flex-col justify-between bg-sidebar p-8 text-sidebar-foreground md:flex">
          <div className="flex items-center gap-2.5">
            <div className="flex size-9 items-center justify-center rounded-md bg-primary">
              <Cpu className="size-5 text-primary-foreground" />
            </div>
            <div className="leading-tight">
              <div className="text-white">IDC Gateway</div>
              <div className="text-[11px] text-sidebar-foreground/60">工业数据采集与协议转换</div>
            </div>
          </div>
          <div className="space-y-3">
            <ShieldCheck className="size-7 text-primary" />
            <h2 className="text-white">协议转换管理控制台</h2>
            <p className="text-sm text-sidebar-foreground/70">
              统一管理工业协议连接、采集点、轮询任务与转换规则，支持配置草稿、校验、发布与回滚。
            </p>
          </div>
          <div className="flex items-center gap-3 text-xs text-sidebar-foreground/60">
            <StatusLight tone="info" label="会话鉴权" />
            <StatusLight tone="info" label="角色权限隔离" />
          </div>
        </div>

        {/* 表单侧 */}
        <div className="bg-card p-8">
          <h2>登录</h2>
          <p className="mt-1 text-sm text-muted-foreground">使用运维账号登录控制台</p>

          {/* 3 / 4. 登录失败 / 后端不可达 */}
          {error && error.code !== "empty" && (
            <Alert
              variant="destructive"
              className="mt-4 border-status-error-border bg-status-error-bg text-status-error"
            >
              {error.code === "unreachable" || error.code === "server_error" ? (
                <ServerCrash className="size-4" />
              ) : (
                <AlertCircle className="size-4" />
              )}
              <AlertTitle>
                {error.code === "unreachable"
                  ? "后端不可达"
                  : error.code === "server_error"
                    ? "服务器错误"
                  : error.code === "rate_limited"
                    ? "登录暂时受限"
                    : "登录失败"}
              </AlertTitle>
              <AlertDescription className="text-status-error/80">{error.message}</AlertDescription>
            </Alert>
          )}

          <form className="mt-5 space-y-4" onSubmit={handleSubmit} noValidate>
            <div className="space-y-1.5">
              <Label htmlFor="acc">账号</Label>
              <Input
                id="acc"
                value={account}
                disabled={submitting}
                aria-invalid={accountEmpty}
                onChange={(e) => setAccount(e.target.value)}
                placeholder="工号 / 用户名"
              />
              {accountEmpty && <p className="text-xs text-status-error">请输入账号</p>}
            </div>
            <div className="space-y-1.5">
              <Label htmlFor="pwd">密码</Label>
              <Input
                id="pwd"
                type="password"
                maxLength={256}
                value={password}
                disabled={submitting}
                aria-invalid={passwordEmpty}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="••••••••"
              />
              {passwordEmpty && <p className="text-xs text-status-error">请输入密码</p>}
            </div>

            {/* 2. 登录中 */}
            <Button type="submit" className="w-full" disabled={submitting}>
              {submitting ? (
                <>
                  <Loader2 className="size-4 animate-spin" />
                  登录中…
                </>
              ) : (
                "登录"
              )}
            </Button>
          </form>

          <p className="mt-4 rounded-md bg-muted/60 px-3 py-2 text-xs text-muted-foreground">
            首次启动时，管理员初始密码由后端控制台输出；也可通过
            <span className="mx-1 font-mono">FIELD_CONSOLE_ADMIN_PASSWORD</span>
            环境变量预设。角色与权限以服务端返回结果为准。
          </p>
        </div>
      </Card>
    </div>
  );
}
