import { useState } from "react";
import {
  ResponsiveContainer,
  AreaChart,
  Area,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip as RTooltip,
  ReferenceLine,
} from "recharts";
import { X, RefreshCw, WifiOff } from "lucide-react";
import type { Datapoint, TrendPoint, WsState } from "../../live";
import { makeTrendHistory } from "../../live";
import { Button } from "../ui/button";
import { Badge } from "../ui/badge";
import { cn } from "../ui/utils";

type Range = "5m" | "15m" | "1h";
const RANGE_POINTS: Record<Range, number> = { "5m": 30, "15m": 90, "1h": 360 };

const COLORS = [
  "var(--chart-1)", "var(--chart-2)", "var(--chart-3)", "var(--chart-4)", "var(--chart-5)",
];

function stats(data: TrendPoint[]) {
  const vals = data.map((d) => d.value);
  const min = Math.min(...vals);
  const max = Math.max(...vals);
  const avg = vals.reduce((a, b) => a + b, 0) / vals.length;
  return { min, max, avg };
}

interface TrendPanelProps {
  points: Datapoint[];
  wsState: WsState;
  onRemove: (id: string) => void;
}

export function TrendPanel({ points, wsState, onRemove }: TrendPanelProps) {
  const [range, setRange] = useState<Range>("15m");

  const series = points.slice(0, 5).map((dp, i) => ({
    dp,
    color: COLORS[i % COLORS.length],
    data: makeTrendHistory(
      typeof dp.value === "number" ? dp.value : 100,
      RANGE_POINTS[range],
    ),
  }));

  const paused = wsState !== "connected";

  return (
    <div className="flex flex-col overflow-hidden rounded-md border border-border bg-card">
      {/* 标题行 */}
      <div className="flex shrink-0 items-center justify-between gap-2 border-b border-border px-4 py-3">
        <div className="flex items-center gap-2">
          <span className="text-sm">实时趋势</span>
          {paused && (
            <span className={cn(
              "flex items-center gap-1 rounded border px-1.5 py-0.5 text-[11px]",
              wsState === "reconnecting"
                ? "border-status-warning-border bg-status-warning-bg text-status-warning"
                : "border-status-error-border bg-status-error-bg text-status-error",
            )}>
              <RefreshCw className={cn("size-3", wsState === "reconnecting" && "animate-spin")} />
              {wsState === "reconnecting" ? "重连中，暂停追加" : "已断开"}
            </span>
          )}
        </div>
        <div className="flex items-center gap-1.5">
          {(["5m", "15m", "1h"] as Range[]).map((r) => (
            <button
              key={r}
              onClick={() => setRange(r)}
              className={cn(
                "rounded px-2 py-1 text-xs transition-colors",
                range === r
                  ? "bg-primary text-primary-foreground"
                  : "text-muted-foreground hover:bg-muted",
              )}
            >{r}</button>
          ))}
        </div>
      </div>

      {/* 图例 */}
      {series.length > 0 && (
        <div className="flex shrink-0 flex-wrap items-center gap-2 border-b border-border px-4 py-2">
          {series.map(({ dp, color }) => (
            <span key={dp.id} className="flex items-center gap-1.5 rounded-full border border-border px-2 py-0.5 text-xs">
              <span className="size-2 rounded-full" style={{ background: color }} />
              {dp.name}
              {dp.unit && <span className="text-muted-foreground">({dp.unit})</span>}
              <button onClick={() => onRemove(dp.id)} className="ml-0.5 text-muted-foreground hover:text-foreground">
                <X className="size-3" />
              </button>
            </span>
          ))}
        </div>
      )}

      {/* 统计摘要 */}
      {series.length > 0 && (
        <div className="flex shrink-0 flex-wrap gap-4 border-b border-border px-4 py-2">
          {series.map(({ dp, color, data }) => {
            const s = stats(data);
            return (
              <div key={dp.id} className="flex items-center gap-3 text-xs">
                <span className="size-2 rounded-full shrink-0" style={{ background: color }} />
                <span className="text-muted-foreground">当前</span>
                <span className="tabular-nums">{typeof dp.value === "number" ? dp.value.toFixed(2) : String(dp.value)}</span>
                <span className="text-muted-foreground">最大</span>
                <span className="tabular-nums text-status-error">{s.max.toFixed(1)}</span>
                <span className="text-muted-foreground">最小</span>
                <span className="tabular-nums text-status-success">{s.min.toFixed(1)}</span>
                <span className="text-muted-foreground">均值</span>
                <span className="tabular-nums">{s.avg.toFixed(1)}</span>
              </div>
            );
          })}
        </div>
      )}

      {/* 图表区 */}
      <div className="relative flex-1 min-h-0">
        {series.length === 0 ? (
          <div className="flex h-48 items-center justify-center text-sm text-muted-foreground">
            点击表格行或"查看趋势"图标添加点位（最多 5 条）
          </div>
        ) : (
          <div className="h-64 w-full px-2 py-2">
            <ResponsiveContainer width="100%" height="100%">
              <AreaChart margin={{ top: 4, right: 8, left: -12, bottom: 0 }}>
                <defs>
                  {series.map(({ dp, color }) => (
                    <linearGradient key={dp.id} id={`g-${dp.id}`} x1="0" y1="0" x2="0" y2="1">
                      <stop offset="0%" stopColor={color} stopOpacity={0.3} />
                      <stop offset="100%" stopColor={color} stopOpacity={0} />
                    </linearGradient>
                  ))}
                </defs>
                <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" vertical={false} />
                <XAxis
                  dataKey="t"
                  data={series[0].data}
                  tick={{ fontSize: 10, fill: "var(--muted-foreground)" }}
                  interval={Math.floor(RANGE_POINTS[range] / 6)}
                  axisLine={false}
                  tickLine={false}
                />
                <YAxis
                  tick={{ fontSize: 10, fill: "var(--muted-foreground)" }}
                  axisLine={false}
                  tickLine={false}
                />
                <RTooltip
                  contentStyle={{ fontSize: 11, borderRadius: 6, border: "1px solid var(--border)" }}
                  labelStyle={{ color: "var(--muted-foreground)" }}
                />
                {series.map(({ dp, color, data }) => (
                  <Area
                    key={dp.id}
                    data={data}
                    type="monotone"
                    dataKey="value"
                    stroke={color}
                    strokeWidth={1.5}
                    fill={`url(#g-${dp.id})`}
                    name={dp.name}
                    dot={false}
                  />
                ))}
              </AreaChart>
            </ResponsiveContainer>
          </div>
        )}

        {/* WS 重连中 overlay — 黄色半透明，不完全遮挡图表 */}
        {wsState === "reconnecting" && series.length > 0 && (
          <div className="pointer-events-none absolute inset-0 flex items-end justify-center pb-6">
            <div className="flex items-center gap-2 rounded-md border border-status-warning-border bg-status-warning-bg/90 px-4 py-2 text-sm text-status-warning shadow-sm backdrop-blur-[1px]">
              <RefreshCw className="size-4 shrink-0 animate-spin" />
              正在重连，趋势图已暂停追加数据
            </div>
          </div>
        )}

        {/* WS 断开 overlay — 红色，较强遮挡 */}
        {wsState === "disconnected" && series.length > 0 && (
          <div className="absolute inset-0 flex items-center justify-center bg-background/70 backdrop-blur-[2px]">
            <div className="flex flex-col items-center gap-2 rounded-md border border-status-error-border bg-card/90 px-6 py-4 text-center shadow-sm">
              <WifiOff className="size-7 text-status-error" />
              <span className="text-sm text-status-error">WebSocket 已断开，趋势暂停更新</span>
              <span className="text-xs text-muted-foreground">显示断开前最后一次数据</span>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
