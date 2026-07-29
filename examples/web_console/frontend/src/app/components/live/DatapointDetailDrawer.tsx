import { type ReactNode, useEffect, useState } from "react";
import { Copy, Loader2, RefreshCw } from "lucide-react";
import { toast } from "sonner";
import {
  Area,
  AreaChart,
  ResponsiveContainer,
  Tooltip as ChartTooltip,
} from "recharts";
import { apiGet } from "../../api";
import type { Datapoint, Quality } from "../../live";
import { STALE_THRESHOLD_S } from "../../live";
import {
  DatapointValueCell,
  DpStatusBadge,
  QualityBadge,
  StaleDataTag,
} from "./atoms";
import { ReadNowResultPanel } from "./ReadNowButton";
import { Badge } from "../ui/badge";
import { Button } from "../ui/button";
import { ScrollArea } from "../ui/scroll-area";
import { Separator } from "../ui/separator";
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from "../ui/sheet";

interface HistoryRow {
  ts: number;
  value_num: number | null;
  value_text: string;
  quality: string;
}

interface HistoryResponse {
  rows: HistoryRow[];
}

function quality(value: string): Quality {
  if (value === "Ok" || value === "Good") return "Good";
  if (value === "Error" || value === "Bad") return "Bad";
  return "Uncertain";
}

function InfoRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <>
      <dt className="text-muted-foreground">{label}</dt>
      <dd>{children}</dd>
    </>
  );
}

export function DatapointDetailDrawer({
  datapoint,
  open,
  onOpenChange,
}: {
  datapoint: Datapoint | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  const [history, setHistory] = useState<HistoryRow[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string | null>(null);

  async function loadHistory() {
    if (!datapoint) return;
    setHistoryLoading(true);
    setHistoryError(null);
    try {
      const now = Date.now();
      const response = await apiGet<HistoryResponse>(
        `/data/history?id=${encodeURIComponent(datapoint.id)}&from=${now - 5 * 60_000}&to=${now}&size=100`,
      );
      setHistory(response.rows);
    } catch (error) {
      setHistoryError(error instanceof Error ? error.message : "历史数据加载失败");
    } finally {
      setHistoryLoading(false);
    }
  }

  useEffect(() => {
    if (open && datapoint) loadHistory();
    else setHistory([]);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, datapoint?.id]);

  if (!datapoint) return null;
  const dp = datapoint;
  const isStale = dp.ageSeconds > STALE_THRESHOLD_S;
  const chronological = [...history].reverse();
  const sparkData = chronological
    .filter((row) => row.value_num !== null)
    .map((row) => ({
      time: new Date(row.ts).toLocaleTimeString("zh-CN", { hour12: false }),
      value: row.value_num,
    }));

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent
        side="right"
        className="flex flex-col gap-0 p-0 sm:max-w-none"
        style={{ width: 500, maxWidth: "94vw" }}
      >
        <SheetHeader className="border-b border-border px-5 py-4">
          <SheetTitle className="flex items-center justify-between">
            {dp.name}
            <Button
              variant="ghost"
              size="icon"
              className="size-7"
              onClick={() => {
                navigator.clipboard.writeText(dp.id);
                toast.success("已复制点位 ID");
              }}
            >
              <Copy className="size-3.5" />
            </Button>
          </SheetTitle>
          <SheetDescription className="font-mono text-xs">
            {dp.address} · {dp.transportName}
          </SheetDescription>
        </SheetHeader>

        <ScrollArea className="min-h-0 flex-1">
          <div className="space-y-5 px-5 py-4">
            <section className="space-y-3">
              <h3 className="text-xs text-muted-foreground">实时状态</h3>
              <div className="rounded-md border border-border bg-muted/20 p-3">
                <div className="flex items-start justify-between gap-3">
                  <DatapointValueCell
                    value={dp.value}
                    dataType={dp.dataType}
                    unit={dp.unit}
                    trend={dp.trend}
                    stale={isStale}
                    justUpdated={dp.justUpdated}
                  />
                  <div className="flex flex-wrap gap-1.5">
                    <QualityBadge quality={dp.quality} />
                    <DpStatusBadge status={dp.status} />
                  </div>
                </div>
                <div className="mt-2 flex items-center gap-3 text-xs text-muted-foreground">
                  <span className="font-mono">{dp.timestamp}</span>
                  <StaleDataTag ageSeconds={dp.ageSeconds} />
                  {isStale && <span className="text-status-warning">数据可能过期</span>}
                </div>

                {sparkData.length > 1 && (
                  <div className="mt-3 border-t border-border pt-3">
                    <p className="mb-1.5 text-[11px] text-muted-foreground">
                      最近 5 分钟真实采样
                    </p>
                    <div className="h-20 w-full">
                      <ResponsiveContainer width="100%" height="100%">
                        <AreaChart data={sparkData}>
                          <ChartTooltip
                            contentStyle={{
                              fontSize: 10,
                              borderRadius: 4,
                              border: "1px solid var(--border)",
                            }}
                          />
                          <Area
                            type="monotone"
                            dataKey="value"
                            stroke="var(--chart-1)"
                            fill="var(--chart-1)"
                            fillOpacity={0.15}
                            dot={false}
                            isAnimationActive={false}
                          />
                        </AreaChart>
                      </ResponsiveContainer>
                    </div>
                  </div>
                )}
              </div>
            </section>

            <Separator />

            <section className="space-y-2">
              <h3 className="text-xs text-muted-foreground">基本信息</h3>
              <dl className="grid grid-cols-[7rem_1fr] gap-y-2 text-sm">
                <InfoRow label="点位 ID"><span className="font-mono">{dp.id}</span></InfoRow>
                <InfoRow label="点位地址"><span className="font-mono">{dp.address}</span></InfoRow>
                <InfoRow label="所属 Transport">{dp.transportName}</InfoRow>
                <InfoRow label="数据类型">{dp.dataType}</InfoRow>
                <InfoRow label="单位">{dp.unit || "—"}</InfoRow>
                <InfoRow label="标签">
                  <span className="flex flex-wrap gap-1">
                    {dp.tags.length
                      ? dp.tags.map((tag) => <Badge key={tag} variant="secondary">{tag}</Badge>)
                      : "—"}
                  </span>
                </InfoRow>
              </dl>
            </section>

            <Separator />

            <section className="space-y-2">
              <div className="flex items-center justify-between">
                <h3 className="text-xs text-muted-foreground">最近采样</h3>
                <Button variant="ghost" size="sm" onClick={loadHistory} disabled={historyLoading}>
                  {historyLoading
                    ? <Loader2 className="size-3.5 animate-spin" />
                    : <RefreshCw className="size-3.5" />}
                  刷新
                </Button>
              </div>
              {historyError ? (
                <div className="rounded-md border border-red-200 bg-red-50 p-3 text-xs text-red-700">
                  {historyError}
                </div>
              ) : (
                <div className="max-h-64 overflow-auto rounded-md border border-border">
                  <table className="w-full text-xs">
                    <thead className="sticky top-0 bg-muted">
                      <tr>
                        <th className="px-3 py-2 text-left">时间</th>
                        <th className="px-3 py-2 text-left">值</th>
                        <th className="px-3 py-2 text-left">质量</th>
                      </tr>
                    </thead>
                    <tbody>
                      {history.map((row) => (
                        <tr key={`${row.ts}-${row.value_text}`} className="border-t border-border">
                          <td className="px-3 py-1.5 font-mono text-muted-foreground">
                            {new Date(row.ts).toLocaleTimeString("zh-CN", { hour12: false })}
                          </td>
                          <td className="px-3 py-1.5 tabular-nums">{row.value_text}</td>
                          <td className="px-3 py-1.5"><QualityBadge quality={quality(row.quality)} /></td>
                        </tr>
                      ))}
                      {!historyLoading && history.length === 0 && (
                        <tr><td colSpan={3} className="h-16 text-center text-muted-foreground">暂无历史采样</td></tr>
                      )}
                    </tbody>
                  </table>
                </div>
              )}
            </section>

            <Separator />

            <section className="space-y-2">
              <h3 className="text-xs text-muted-foreground">刷新运行时快照</h3>
              <ReadNowResultPanel datapointId={dp.id} />
            </section>
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  );
}
