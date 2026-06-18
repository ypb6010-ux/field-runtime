import { Loader2, CheckCircle2, XCircle, Gauge, Clock, Server, ListChecks } from "lucide-react";
import type { TestResult } from "../../transports";

export type TestStatus = "idle" | "missing" | "loading" | "success" | "error";

export interface TestState {
  status: TestStatus;
  result?: TestResult;
  missing?: string[];
  endpoint?: string;
}

/** 测试连接结果面板（Drawer 区域 D）。按钮在 Drawer 底部触发。 */
export function TestConnectionPanel({ state }: { state: TestState }) {
  if (state.status === "idle") {
    return (
      <div className="rounded-md border border-dashed border-border bg-muted/20 px-4 py-3 text-sm text-muted-foreground">
        尚未测试。完成配置后点击底部「测试连接」验证可达性与认证。
      </div>
    );
  }

  if (state.status === "missing") {
    return (
      <div className="rounded-md border border-status-warning-border bg-status-warning-bg px-4 py-3 text-sm text-status-warning">
        请先完成必填项后再测试：
        <span className="font-medium">{state.missing?.join("、")}</span>
      </div>
    );
  }

  if (state.status === "loading") {
    return (
      <div className="space-y-1 rounded-md border border-border bg-muted/30 px-4 py-3">
        <div className="flex items-center gap-2 text-sm text-muted-foreground">
          <Loader2 className="size-4 animate-spin text-primary" />
          正在测试连接…
        </div>
        {state.endpoint && (
          <p className="pl-6 text-xs text-muted-foreground">
            正在连接 <span className="font-mono">{state.endpoint}</span>
          </p>
        )}
      </div>
    );
  }

  const r = state.result!;
  if (state.status === "success") {
    return (
      <div className="space-y-2 rounded-md border border-status-success-border bg-status-success-bg px-4 py-3">
        <div className="flex items-center gap-2 text-sm text-status-success">
          <CheckCircle2 className="size-4" />
          测试成功
        </div>
        <dl className="grid grid-cols-2 gap-x-4 gap-y-1 text-xs text-foreground/80">
          <span className="flex items-center gap-1.5 text-muted-foreground"><Gauge className="size-3" /> 延迟</span>
          <span className="tabular-nums">{r.latencyMs} ms</span>
          <span className="flex items-center gap-1.5 text-muted-foreground"><Server className="size-3" /> 目标</span>
          <span className="truncate font-mono">{r.endpoint}</span>
          <span className="flex items-center gap-1.5 text-muted-foreground"><Clock className="size-3" /> 时间</span>
          <span className="tabular-nums">{r.at}</span>
        </dl>
        <p className="text-xs text-muted-foreground">{r.message}</p>
      </div>
    );
  }

  // error
  return (
    <div className="space-y-3 rounded-md border border-status-error-border bg-status-error-bg px-4 py-3">
      {/* 标题行 */}
      <div className="flex items-start justify-between gap-2">
        <div className="flex items-center gap-2 text-sm text-status-error">
          <XCircle className="mt-0.5 size-4 shrink-0" />
          <span>测试失败</span>
        </div>
        {r.errorType && (
          <span className="rounded border border-status-error-border bg-card/60 px-2 py-0.5 font-mono text-xs text-status-error">
            {r.errorType}
          </span>
        )}
      </div>

      {/* 字段详情（与成功态对称的标签行） */}
      <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-xs">
        <span className="flex items-center gap-1.5 text-muted-foreground">
          <Server className="size-3" /> endpoint
        </span>
        <span className="truncate font-mono text-foreground/80">{r.endpoint}</span>

        <span className="flex items-center gap-1.5 text-muted-foreground">
          <Clock className="size-3" /> time
        </span>
        <span className="tabular-nums text-foreground/80">{r.at}</span>

        <span className="flex items-center gap-1.5 text-muted-foreground">
          <XCircle className="size-3" /> message
        </span>
        <span className="font-mono text-foreground/80 break-all">{r.message}</span>
      </dl>

      {/* 建议检查 */}
      {r.suggestions && (
        <div className="rounded-md border border-status-error-border/40 bg-card/50 px-3 py-2.5">
          <div className="mb-2 flex items-center gap-1.5 text-xs text-foreground">
            <ListChecks className="size-3.5" />
            建议检查以下项目
          </div>
          <ul className="space-y-1 text-xs text-muted-foreground">
            {r.suggestions.map((s) => (
              <li key={s} className="flex items-start gap-2">
                <span className="mt-1.5 size-1 shrink-0 rounded-full bg-status-error/60" />
                {s}
              </li>
            ))}
          </ul>
        </div>
      )}
    </div>
  );
}
