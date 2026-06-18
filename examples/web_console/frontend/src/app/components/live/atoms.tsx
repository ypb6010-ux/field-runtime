/**
 * Live 页面原子组件：QualityBadge / StaleDataTag / DatapointValueCell
 */
import { ArrowUp, ArrowDown, Minus } from "lucide-react";
import type { Quality, DpStatus, Trend, DataType } from "../../live";
import { STALE_THRESHOLD_S } from "../../live";
import { Badge } from "../ui/badge";
import { cn } from "../ui/utils";

// ---- QualityBadge ----
const Q_STYLE: Record<Quality, string> = {
  Good: "border-status-success-border bg-status-success-bg text-status-success",
  Bad: "border-status-error-border bg-status-error-bg text-status-error",
  Uncertain: "border-status-warning-border bg-status-warning-bg text-status-warning",
};

export function QualityBadge({ quality }: { quality: Quality }) {
  return (
    <Badge variant="outline" className={cn("text-[11px]", Q_STYLE[quality])}>
      {quality}
    </Badge>
  );
}

// ---- Status Badge ----
const S_STYLE: Record<DpStatus, string> = {
  normal: "border-status-success-border bg-status-success-bg text-status-success",
  alarm: "border-status-warning-border bg-status-warning-bg text-status-warning",
  error: "border-status-error-border bg-status-error-bg text-status-error",
  offline: "border-border bg-muted text-status-disabled",
  stale: "border-status-warning-border bg-status-warning-bg/50 text-status-warning",
};
const S_LABEL: Record<DpStatus, string> = {
  normal: "正常", alarm: "告警", error: "错误", offline: "离线", stale: "过期",
};

export function DpStatusBadge({ status }: { status: DpStatus }) {
  return (
    <Badge variant="outline" className={cn("gap-1 text-[11px]", S_STYLE[status])}>
      <span className={cn(
        "size-1.5 rounded-full",
        status === "normal" ? "bg-status-success"
        : status === "alarm" ? "bg-status-warning"
        : status === "error" ? "bg-status-error"
        : status === "stale" ? "bg-status-warning animate-pulse"
        : "bg-status-disabled"
      )} />
      {S_LABEL[status]}
    </Badge>
  );
}

// ---- StaleDataTag ----
export function StaleDataTag({ ageSeconds }: { ageSeconds: number }) {
  if (ageSeconds <= STALE_THRESHOLD_S) return null;
  return (
    <span className="ml-1 rounded-sm border border-status-warning-border bg-status-warning-bg px-1 py-0.5 font-mono text-[10px] text-status-warning">
      {ageSeconds}s 前
    </span>
  );
}

// ---- DatapointValueCell ----
const TREND_ICON: Record<Trend, typeof ArrowUp> = {
  up: ArrowUp, down: ArrowDown, flat: Minus,
};
const TREND_COLOR: Record<Trend, string> = {
  up: "text-status-success", down: "text-status-error", flat: "text-muted-foreground",
};

export function DatapointValueCell({
  value, dataType, unit, trend, stale, justUpdated,
}: {
  value: number | boolean | string;
  dataType: DataType;
  unit: string;
  trend: Trend;
  stale: boolean;
  justUpdated?: boolean;
}) {
  const TrendIcon = TREND_ICON[trend];

  const display =
    dataType === "boolean"
      ? (value ? "ON" : "OFF")
      : dataType === "string"
        ? String(value)
        : typeof value === "number"
          ? Number.isInteger(value) ? String(value) : value.toFixed(2)
          : String(value);

  const isOn = dataType === "boolean" && value === true;
  const isOff = dataType === "boolean" && value === false;

  return (
    <div
      className={cn(
        "flex items-center gap-1 rounded px-1 py-0.5 transition-colors duration-500",
        justUpdated && "bg-primary/8",
        stale && "opacity-60",
      )}
    >
      <span
        className={cn(
          "tabular-nums",
          dataType === "number" ? "font-semibold" : "",
          isOn ? "text-status-success font-semibold"
          : isOff ? "text-status-disabled"
          : dataType === "string" ? "font-mono text-xs text-muted-foreground"
          : "",
        )}
      >
        {display}
      </span>
      {unit && (
        <span className="text-xs text-muted-foreground">{unit}</span>
      )}
      {dataType === "number" && (
        <TrendIcon className={cn("size-3 shrink-0", TREND_COLOR[trend])} />
      )}
    </div>
  );
}
