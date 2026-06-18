import { type ReactNode, useMemo } from "react";
import { Copy, ExternalLink } from "lucide-react";
import { toast } from "sonner";
import { ResponsiveContainer, AreaChart, Area, Tooltip as RTooltip } from "recharts";
import type { Datapoint } from "../../live";
import { STALE_THRESHOLD_S, makeTrendHistory } from "../../live";
import { QualityBadge, DpStatusBadge, DatapointValueCell, StaleDataTag } from "./atoms";
import { ReadNowResultPanel } from "./ReadNowButton";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription } from "../ui/sheet";
import { Button } from "../ui/button";
import { Badge } from "../ui/badge";
import { Separator } from "../ui/separator";
import { ScrollArea } from "../ui/scroll-area";

// 最近 10 条值变化（mock）
function mockHistory(dp: Datapoint) {
  const vals = [dp.value];
  for (let i = 1; i < 10; i++) {
    const prev = typeof vals[i - 1] === "number"
      ? parseFloat((Number(vals[i - 1]) * (0.96 + Math.random() * 0.08)).toFixed(2))
      : vals[i - 1];
    vals.push(prev);
  }
  return vals.map((v, i) => {
    const d = new Date(2026, 5, 18, 10, 42, 18 - i * 15);
    return {
      time: d.toLocaleTimeString("zh-CN", { hour12: false }),
      value: v,
      quality: i < 2 ? dp.quality : "Good" as const,
    };
  });
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
  datapoint, open, onOpenChange, canControl,
}: {
  datapoint: Datapoint | null;
  open: boolean;
  onOpenChange: (o: boolean) => void;
  canControl: boolean;
}) {
  if (!datapoint) return null;
  const dp = datapoint;
  const isStale = dp.ageSeconds > STALE_THRESHOLD_S;
  const history = mockHistory(dp);
  // mini sparkline：最近 5 分钟（30 个采样点）
  // eslint-disable-next-line react-hooks/rules-of-hooks
  const sparkData = useMemo(
    () => dp.dataType === "number"
      ? makeTrendHistory(typeof dp.value === "number" ? dp.value : 100, 30)
      : null,
    // dp.id 变化时重新生成
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [dp.id, dp.dataType],
  );

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent side="right" className="flex flex-col gap-0 p-0 sm:max-w-none" style={{ width: 480, maxWidth: "94vw" }}>
        <SheetHeader className="border-b border-border px-5 py-4">
          <SheetTitle className="flex items-center justify-between">
            {dp.name}
            <Button
              variant="ghost" size="icon" className="size-7"
              onClick={() => { navigator.clipboard.writeText(dp.id); toast.success("已复制点位 ID"); }}
            >
              <Copy className="size-3.5" />
            </Button>
          </SheetTitle>
          <SheetDescription className="font-mono text-xs">{dp.address} · {dp.transportName}</SheetDescription>
        </SheetHeader>

        <ScrollArea className="min-h-0 flex-1">
          <div className="space-y-5 px-5 py-4">
            {/* 实时状态 */}
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

                {/* Mini sparkline — 最近 5 分钟趋势 */}
                {sparkData && (
                  <div className="mt-3 border-t border-border pt-3">
                    <p className="mb-1.5 text-[11px] text-muted-foreground">最近 5 分钟趋势</p>
                    <div className="h-16 w-full">
                      <ResponsiveContainer width="100%" height="100%">
                        <AreaChart data={sparkData} margin={{ top: 2, right: 2, left: -32, bottom: 0 }}>
                          <defs>
                            <linearGradient id="spark-grad" x1="0" y1="0" x2="0" y2="1">
                              <stop offset="0%" stopColor="var(--chart-1)" stopOpacity={0.4} />
                              <stop offset="100%" stopColor="var(--chart-1)" stopOpacity={0} />
                            </linearGradient>
                          </defs>
                          <RTooltip
                            contentStyle={{ fontSize: 10, borderRadius: 4, border: "1px solid var(--border)", padding: "2px 6px" }}
                            labelStyle={{ display: "none" }}
                            formatter={(v: number) => [v.toFixed(2), dp.unit || "value"]}
                          />
                          <Area
                            type="monotone"
                            dataKey="value"
                            stroke="var(--chart-1)"
                            strokeWidth={1.5}
                            fill="url(#spark-grad)"
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

            {/* 基本信息 */}
            <section className="space-y-2">
              <h3 className="text-xs text-muted-foreground">基本信息</h3>
              <dl className="grid grid-cols-[7rem_1fr] gap-y-2 text-sm">
                <InfoRow label="点位地址"><span className="font-mono">{dp.address}</span></InfoRow>
                <InfoRow label="所属 Transport">{dp.transportName}</InfoRow>
                <InfoRow label="数据类型">{dp.dataType}</InfoRow>
                <InfoRow label="单位">{dp.unit || "—"}</InfoRow>
                <InfoRow label="标签">
                  <span className="flex flex-wrap gap-1">
                    {dp.tags.length ? dp.tags.map((t) => <Badge key={t} variant="secondary">{t}</Badge>) : "—"}
                  </span>
                </InfoRow>
              </dl>
            </section>

            <Separator />

            {/* 最近值变化 */}
            <section className="space-y-2">
              <h3 className="text-xs text-muted-foreground">最近值变化（最近 10 条）</h3>
              <div className="overflow-hidden rounded-md border border-border">
                <table className="w-full text-xs">
                  <thead className="bg-muted/50">
                    <tr>
                      <th className="px-3 py-2 text-left text-muted-foreground">时间</th>
                      <th className="px-3 py-2 text-left text-muted-foreground">值</th>
                      <th className="px-3 py-2 text-left text-muted-foreground">质量</th>
                    </tr>
                  </thead>
                  <tbody>
                    {history.map((h, i) => (
                      <tr key={i} className="border-t border-border">
                        <td className="px-3 py-1.5 font-mono text-muted-foreground">{h.time}</td>
                        <td className="px-3 py-1.5 tabular-nums">{String(h.value)}</td>
                        <td className="px-3 py-1.5"><QualityBadge quality={h.quality} /></td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </section>

            <Separator />

            {/* 立即拉取 */}
            <section className="space-y-2">
              <h3 className="text-xs text-muted-foreground">立即拉取 POST /data/read</h3>
              <ReadNowResultPanel canControl={canControl} datapointId={dp.id} />
            </section>

            <Separator />

            {/* 操作 */}
            <section className="flex flex-wrap gap-2">
              <Button variant="outline" size="sm" className="gap-1.5">
                <ExternalLink className="size-3.5" />
                查看历史
              </Button>
              <Button
                variant="outline" size="sm" className="gap-1.5"
                onClick={() => { navigator.clipboard.writeText(dp.id); toast.success("已复制点位 ID"); }}
              >
                <Copy className="size-3.5" />
                复制点位 ID
              </Button>
            </section>
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  );
}
