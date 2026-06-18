import { useState } from "react";
import { Loader2, Zap, CheckCircle2, XCircle, Gauge, Clock, ListChecks } from "lucide-react";
import type { ReadResult } from "../../live";
import { readDatapoint } from "../../live";
import { Button } from "../ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";
import { cn } from "../ui/utils";

type ReadStatus = "idle" | "loading" | "success" | "error";

interface ReadNowState {
  status: ReadStatus;
  result?: ReadResult;
}

/** 立即拉取按钮（行内小图标按钮，结果显示在 ReadNowResultPanel）*/
export function ReadNowButton({
  datapointId,
  canControl,
  wsDisconnected,
  variant = "icon",
  onResult,
}: {
  datapointId: string;
  canControl: boolean;
  wsDisconnected?: boolean;
  variant?: "icon" | "full";
  onResult?: (r: ReadResult) => void;
}) {
  const [loading, setLoading] = useState(false);

  async function handleClick() {
    setLoading(true);
    const r = await readDatapoint(datapointId);
    setLoading(false);
    onResult?.(r);
  }

  if (!canControl) {
    return (
      <Tooltip>
        <TooltipTrigger asChild>
          <span className="inline-flex">
            <Button variant="ghost" size={variant === "icon" ? "icon" : "sm"} disabled
              className={cn(variant === "icon" && "size-7", "opacity-50 cursor-not-allowed")}>
              <Zap className="size-3.5" />
              {variant === "full" && " 立即拉取"}
            </Button>
          </span>
        </TooltipTrigger>
        <TooltipContent>无控制权限，请联系管理员</TooltipContent>
      </Tooltip>
    );
  }

  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <Button
          variant="ghost"
          size={variant === "icon" ? "icon" : "sm"}
          disabled={loading}
          className={cn(variant === "icon" && "size-7", "gap-1.5")}
          onClick={handleClick}
        >
          {loading
            ? <Loader2 className="size-3.5 animate-spin" />
            : <Zap className="size-3.5" />}
          {variant === "full" && (loading ? "拉取中…" : "立即拉取")}
        </Button>
      </TooltipTrigger>
      <TooltipContent>
        {wsDisconnected ? "WS 已断开，将通过 HTTP 接口拉取" : "立即拉取最新值"}
      </TooltipContent>
    </Tooltip>
  );
}

/** ReadNowResultPanel — 展示在 DatapointDetailDrawer 中的拉取结果 */
export function ReadNowResultPanel({
  canControl, datapointId,
}: {
  canControl: boolean;
  datapointId: string;
}) {
  const [state, setState] = useState<ReadNowState>({ status: "idle" });

  async function handleRead() {
    setState({ status: "loading" });
    const result = await readDatapoint(datapointId);
    setState({ status: result.ok ? "success" : "error", result });
  }

  return (
    <div className="space-y-2">
      {/* 拉取按钮 */}
      {canControl ? (
        <Button
          variant="outline"
          size="sm"
          className="gap-1.5"
          disabled={state.status === "loading"}
          onClick={handleRead}
        >
          {state.status === "loading"
            ? <Loader2 className="size-3.5 animate-spin" />
            : <Zap className="size-3.5" />}
          {state.status === "loading" ? "拉取中…" : "立即拉取"}
        </Button>
      ) : (
        <Tooltip>
          <TooltipTrigger asChild>
            <span className="inline-flex">
              <Button variant="outline" size="sm" disabled className="gap-1.5 cursor-not-allowed">
                <Zap className="size-3.5" />
                立即拉取
              </Button>
            </span>
          </TooltipTrigger>
          <TooltipContent>无控制权限，请联系管理员</TooltipContent>
        </Tooltip>
      )}

      {/* 结果展示 */}
      {state.status === "idle" && (
        <div className="rounded-md border border-dashed border-border bg-muted/20 px-3 py-2.5 text-xs text-muted-foreground">
          点击"立即拉取"通过 POST /data/read 获取最新值
        </div>
      )}

      {state.status === "loading" && (
        <div className="flex items-center gap-2 rounded-md border border-border bg-muted/30 px-3 py-2.5 text-xs text-muted-foreground">
          <Loader2 className="size-3.5 animate-spin text-primary" />
          正在读取…
        </div>
      )}

      {state.status === "success" && state.result && (
        <div className="space-y-2 rounded-md border border-status-success-border bg-status-success-bg px-3 py-2.5">
          <div className="flex items-center gap-1.5 text-xs text-status-success">
            <CheckCircle2 className="size-3.5" />
            读取成功
          </div>
          <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 text-xs">
            <dt className="text-muted-foreground">value</dt>
            <dd className="font-semibold">{String(state.result.value)}</dd>
            <dt className="text-muted-foreground">quality</dt>
            <dd>{state.result.quality}</dd>
            <dt className="flex items-center gap-1 text-muted-foreground"><Gauge className="size-3" /> latency</dt>
            <dd className="tabular-nums">{state.result.latencyMs} ms</dd>
            <dt className="flex items-center gap-1 text-muted-foreground"><Clock className="size-3" /> time</dt>
            <dd className="tabular-nums">{state.result.timestamp}</dd>
          </dl>
        </div>
      )}

      {state.status === "error" && state.result && (
        <div className="space-y-2 rounded-md border border-status-error-border bg-status-error-bg px-3 py-2.5">
          <div className="flex items-center justify-between gap-2">
            <div className="flex items-center gap-1.5 text-xs text-status-error">
              <XCircle className="size-3.5" />
              读取失败
            </div>
            {state.result.errorType && (
              <span className="rounded border border-status-error-border bg-card/60 px-1.5 py-0.5 font-mono text-[10px] text-status-error">
                {state.result.errorType}
              </span>
            )}
          </div>
          <p className="text-xs text-foreground/80">{state.result.message}</p>
          {state.result.suggestions && (
            <div className="rounded border border-status-error-border/40 bg-card/50 px-2.5 py-2">
              <div className="mb-1 flex items-center gap-1 text-[11px] text-foreground">
                <ListChecks className="size-3" /> 建议检查
              </div>
              <ul className="space-y-0.5 text-[11px] text-muted-foreground">
                {state.result.suggestions.map((s) => (
                  <li key={s} className="flex items-start gap-1.5">
                    <span className="mt-1.5 size-1 shrink-0 rounded-full bg-status-error/50" />{s}
                  </li>
                ))}
              </ul>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
