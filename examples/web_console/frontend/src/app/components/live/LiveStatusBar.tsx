import { Wifi, WifiOff, RefreshCw, Activity, AlertTriangle, XCircle, PauseCircle } from "lucide-react";
import type { WsState } from "../../live";
import { cn } from "../ui/utils";

const WS_META: Record<WsState, { icon: typeof Wifi; dot: string; label: string; text: string }> = {
  connected: { icon: Wifi, dot: "bg-status-success", label: "WS Connected", text: "text-status-success" },
  reconnecting: { icon: RefreshCw, dot: "bg-status-warning animate-pulse", label: "Reconnecting", text: "text-status-warning" },
  disconnected: { icon: WifiOff, dot: "bg-status-error", label: "Disconnected", text: "text-status-error" },
};

interface LiveStatusBarProps {
  wsState: WsState;
  lastUpdate: string;
  subscriptions: number;
  updateRate: number;
  staleCount: number;
}

function Stat({ label, value, highlight }: { label: string; value: string | number; highlight?: string }) {
  return (
    <span className="flex items-center gap-1.5 text-xs">
      <span className="text-muted-foreground">{label}</span>
      <span className={cn("tabular-nums", highlight)}>{value}</span>
    </span>
  );
}

/** LiveStatusBar — 顶部状态条，WebSocket 连接摘要 */
export function LiveStatusBar({
  wsState, lastUpdate, subscriptions, updateRate, staleCount,
}: LiveStatusBarProps) {
  const m = WS_META[wsState];
  const Icon = m.icon;

  return (
    <div className="flex flex-wrap items-center gap-4 rounded-md border border-border bg-card px-4 py-2.5">
      {/* WS 状态 */}
      <span className={cn("flex items-center gap-2 text-xs font-medium", m.text)}>
        <span className="relative inline-flex size-2.5">
          <span className={cn("absolute inline-flex size-2.5 rounded-full", wsState === "reconnecting" && "animate-ping", m.dot, "opacity-50")} />
          <span className={cn("relative inline-flex size-2 rounded-full", m.dot)} />
        </span>
        <Icon className={cn("size-3.5", wsState === "reconnecting" && "animate-spin")} />
        {m.label}
      </span>

      <span className="h-4 w-px bg-border" />

      <Stat label="最近更新" value={lastUpdate} />
      <Stat label="订阅" value={`${subscriptions} 个点位`} />
      <Stat label="速率" value={`${updateRate} msg/s`} />
      {staleCount > 0 && (
        <Stat
          label="过期点位"
          value={staleCount}
          highlight="text-status-warning"
        />
      )}

      {/* 右侧：实时跳动指示 */}
      <span className="ml-auto flex items-center gap-1.5 text-xs text-muted-foreground">
        <Activity className={cn("size-3.5", wsState === "connected" ? "text-status-success" : "text-muted-foreground")} />
        实时
      </span>
    </div>
  );
}

// ---- PausedBanner — 订阅已暂停（WS 仍连接） ----
export function PausedBanner() {
  return (
    <div className="flex items-center gap-2.5 rounded-md border border-border bg-muted/60 px-4 py-2.5 text-sm text-foreground/80">
      <PauseCircle className="size-4 shrink-0 text-muted-foreground" />
      订阅已暂停，当前表格保留最后一次数据。点击「恢复订阅」重新接收实时更新。
    </div>
  );
}

// ---- WsStatusBanner ----
export function WsStatusBanner({ wsState }: { wsState: WsState }) {
  if (wsState === "connected") return null;

  const reconnecting = wsState === "reconnecting";
  return (
    <div className={cn(
      "flex items-center gap-2.5 rounded-md border px-4 py-2.5 text-sm",
      reconnecting
        ? "border-status-warning-border bg-status-warning-bg text-status-warning"
        : "border-status-error-border bg-status-error-bg text-status-error",
    )}>
      {reconnecting
        ? <RefreshCw className="size-4 shrink-0 animate-spin" />
        : <XCircle className="size-4 shrink-0" />}
      <span>
        {reconnecting
          ? "实时连接正在重连，当前数据可能不是最新值。"
          : "实时连接已断开，正在显示最后一次收到的数据。"}
      </span>
    </div>
  );
}

// ---- SubscriptionErrorBanner ----
export function SubscriptionErrorBanner({
  count,
  reasons,
  onRetry,
  onDismiss,
}: {
  count: number;
  reasons?: string[];
  onRetry?: () => void;
  onDismiss?: () => void;
}) {
  return (
    <div className="rounded-md border border-status-warning-border bg-status-warning-bg px-4 py-3">
      <div className="flex items-start justify-between gap-3">
        <div className="flex items-start gap-2">
          <AlertTriangle className="mt-0.5 size-4 shrink-0 text-status-warning" />
          <div className="space-y-1">
            <p className="text-sm text-status-warning">
              {count} 个点位订阅失败，已回退到 latest 数据，实时性可能受影响。
            </p>
            {reasons && reasons.length > 0 && (
              <p className="text-xs text-status-warning/80">
                原因：{reasons.join(" / ")}
              </p>
            )}
          </div>
        </div>
        <div className="flex shrink-0 items-center gap-2">
          {onRetry && (
            <button
              onClick={onRetry}
              className="text-xs text-status-warning underline-offset-2 hover:underline"
            >
              重试订阅
            </button>
          )}
          <button
            onClick={() => {}}
            className="text-xs text-status-warning/70 underline-offset-2 hover:underline"
          >
            查看详情
          </button>
          {onDismiss && (
            <button
              onClick={onDismiss}
              className="text-xs text-status-warning/60 hover:text-status-warning"
            >
              忽略
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
