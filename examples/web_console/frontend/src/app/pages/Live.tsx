import { useEffect, useMemo, useRef, useState } from "react";
import { toast } from "sonner";
import {
  RefreshCw, Zap, Pause, Play, Settings2, AlertCircle, RotateCcw,
} from "lucide-react";
import type { Datapoint, WsState } from "../live";
import {
  SEED_DATAPOINTS, STALE_THRESHOLD_S, simWsTick, readDatapoint,
} from "../live";
import type { Role } from "../types";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { LatestDataErrorState, LiveEmptyState } from "../components/live/LiveEmptyError";
import { Button } from "../components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "../components/ui/tooltip";
import { LiveStatusBar, WsStatusBanner, PausedBanner, SubscriptionErrorBanner } from "../components/live/LiveStatusBar";
import { LiveFilterBar, type LiveFilters, EMPTY_LIVE_FILTERS } from "../components/live/LiveFilterBar";
import { RealTimeDatapointTable } from "../components/live/RealTimeDatapointTable";
import { TrendPanel } from "../components/live/TrendPanel";
import { DatapointDetailDrawer } from "../components/live/DatapointDetailDrawer";

interface LivePageProps {
  role: Role;
}

const CAN_CONTROL: Role[] = ["operator", "admin"];

export function Live({ role }: LivePageProps) {
  const canControl = CAN_CONTROL.includes(role);

  /* ---- data ---- */
  const [datapoints, setDatapoints] = useState<Datapoint[]>(SEED_DATAPOINTS);
  const [latestError, setLatestError] = useState(false);
  const [filters, setFilters] = useState<LiveFilters>(EMPTY_LIVE_FILTERS);

  /* ---- WS simulation ---- */
  const [wsState, setWsState] = useState<WsState>("connected");
  const [paused, setPaused] = useState(false);
  const [updateRate, setUpdateRate] = useState(320);
  const [subscriptionError, setSubscriptionError] = useState(false);
  const tickRef = useRef<ReturnType<typeof setInterval> | null>(null);

  /* ---- UI ---- */
  // 模拟哪些点位订阅失败（真实环境由 WS 错误回调填充）
  const failedSubIds = subscriptionError ? new Set(["dp8", "dp9"]) : undefined;
  const [trendIds, setTrendIds] = useState<string[]>(["dp1", "dp6"]);
  const [detail, setDetail] = useState<{ open: boolean; dp: Datapoint | null }>({ open: false, dp: null });
  const [lastUpdate, setLastUpdate] = useState("10:42:18");
  const [subErrDismissed, setSubErrDismissed] = useState(false);

  /* ---- WS tick loop ---- */
  useEffect(() => {
    if (paused || wsState !== "connected") {
      if (tickRef.current) clearInterval(tickRef.current);
      return;
    }
    tickRef.current = setInterval(() => {
      setDatapoints((prev) => simWsTick(prev));
      setLastUpdate(new Date().toLocaleTimeString("zh-CN", { hour12: false }));
      setUpdateRate(280 + Math.floor(Math.random() * 80));
    }, 2000);
    return () => { if (tickRef.current) clearInterval(tickRef.current); };
  }, [paused, wsState]);

  /* ---- Simulate reconnecting cycle ---- */
  useEffect(() => {
    // 每 20s 模拟一次短暂重连 (2s → 恢复)
    const id = setInterval(() => {
      setWsState((prev) => {
        if (prev !== "connected") return prev;
        setTimeout(() => setWsState("connected"), 2200);
        return "reconnecting";
      });
    }, 20000);
    return () => clearInterval(id);
  }, []);

  /* ---- filter ---- */
  const staleCount = datapoints.filter((d) => d.ageSeconds > STALE_THRESHOLD_S).length;

  const filtered = useMemo(() => {
    const kw = filters.keyword.toLowerCase();
    return datapoints.filter((dp) => {
      if (filters.transportId !== "all" && dp.transportId !== filters.transportId) return false;
      if (filters.status !== "all" && dp.status !== filters.status) return false;
      if (filters.dataType !== "all" && dp.dataType !== filters.dataType) return false;
      if (filters.quality !== "all" && dp.quality !== filters.quality) return false;
      if (filters.onlyChanging && dp.trend === "flat") return false;
      if (kw && !`${dp.name} ${dp.address} ${dp.tags.join(" ")}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [datapoints, filters]);

  const isFiltering = JSON.stringify(filters) !== JSON.stringify(EMPTY_LIVE_FILTERS);
  const trendDps = datapoints.filter((d) => trendIds.includes(d.id));

  function addToTrend(dp: Datapoint) {
    setTrendIds((prev) => prev.includes(dp.id) || prev.length >= 5 ? prev : [...prev, dp.id]);
  }

  async function handleReadNow(dp: Datapoint) {
    const r = await readDatapoint(dp.id);
    if (r.ok) {
      setDatapoints((prev) => prev.map((d) =>
        d.id === dp.id ? { ...d, value: r.value!, quality: r.quality!, timestamp: r.timestamp!, ageSeconds: 0, justUpdated: true } : d,
      ));
      toast.success(`${dp.name} 读取成功 · ${r.latencyMs}ms`);
    } else {
      toast.error(`${dp.name} 读取失败`, { description: r.message });
    }
  }

  return (
    <>
      <PageHeader
        title="实时监控"
        en="Live"
        description="点位实时值、质量码与趋势，数据通过 WebSocket 实时刷新。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5"
              onClick={() => { setDatapoints(SEED_DATAPOINTS); setLatestError(false); toast.message("已刷新"); }}>
              <RefreshCw className="size-3.5" />
              刷新最新值
            </Button>
            <PermissionButton
              allowed={canControl}
              deniedHint="无控制权限，请联系管理员"
              onAction={() => toast.message("批量立即拉取…")}
              size="sm"
            >
              <Zap className="size-3.5" />
              立即拉取
            </PermissionButton>
            {wsState === "disconnected" ? (
              <Button variant="outline" size="sm" className="gap-1.5"
                onClick={() => { setWsState("reconnecting"); setTimeout(() => setWsState("connected"), 2200); }}>
                <RotateCcw className="size-3.5" />
                重新订阅
              </Button>
            ) : (
              <Button variant="outline" size="sm" className="gap-1.5"
                onClick={() => setPaused((p) => !p)}>
                {paused ? <Play className="size-3.5" /> : <Pause className="size-3.5" />}
                {paused ? "恢复订阅" : "暂停订阅"}
              </Button>
            )}
            <Tooltip>
              <TooltipTrigger asChild>
                <Button variant="ghost" size="icon" className="size-8">
                  <Settings2 className="size-4" />
                </Button>
              </TooltipTrigger>
              <TooltipContent>设置列</TooltipContent>
            </Tooltip>
          </>
        }
      />

      <div className="flex flex-col gap-4 p-6">
        {/* WS Banner */}
        <WsStatusBanner wsState={wsState} />

        {/* Paused banner — WS 正常但订阅已暂停 */}
        {paused && wsState === "connected" && <PausedBanner />}

        {/* Subscription Error */}
        {subscriptionError && !subErrDismissed && (
          <SubscriptionErrorBanner
            count={2}
            reasons={["topic permission denied", "transport unavailable"]}
            onRetry={() => { setSubscriptionError(false); toast.success("已重新订阅"); }}
            onDismiss={() => setSubErrDismissed(true)}
          />
        )}

        {/* Status Bar */}
        <LiveStatusBar
          wsState={wsState}
          lastUpdate={lastUpdate}
          subscriptions={datapoints.length}
          updateRate={paused ? 0 : updateRate}
          staleCount={staleCount}
        />

        {/* Filter Bar */}
        <LiveFilterBar
          filters={filters}
          onChange={setFilters}
          onReset={() => setFilters(EMPTY_LIVE_FILTERS)}
        />

        {/* /data/latest 失败 */}
        {latestError ? (
          <LatestDataErrorState
            onRetry={() => { setLatestError(false); toast.success("已重新加载"); }}
          />
        ) : filtered.length === 0 ? (
          /* Empty */
          <LiveEmptyState
            isFiltering={isFiltering}
            canConfig={role === "admin"}
            onReset={() => setFilters(EMPTY_LIVE_FILTERS)}
          />
        ) : (
          <>
            {/* 条目计数 */}
            <div className="flex items-center gap-2 text-xs text-muted-foreground">
              <span>{filtered.length} 个点位{isFiltering ? "（已筛选）" : ""}</span>
              {staleCount > 0 && wsState !== "connected" && (
                <span className="flex items-center gap-1 text-status-warning">
                  <AlertCircle className="size-3" />
                  {staleCount} 个点位数据已过期
                </span>
              )}
            </div>

            {/* 主表格 */}
            <RealTimeDatapointTable
              rows={filtered}
              canControl={canControl}
              wsState={wsState}
              failedSubIds={failedSubIds}
              selectedId={detail.dp?.id}
              onRowClick={(dp) => { setDetail({ open: true, dp }); addToTrend(dp); }}
              onTrend={addToTrend}
              onReadNow={(dp, r) => {
                if (r.ok) {
                  setDatapoints((prev) => prev.map((d) =>
                    d.id === dp.id ? { ...d, value: r.value!, quality: r.quality!, timestamp: r.timestamp!, ageSeconds: 0, justUpdated: true } : d,
                  ));
                  toast.success(`${dp.name} 读取成功 · ${r.latencyMs}ms`);
                } else {
                  toast.error(`${dp.name} 读取失败`, { description: r.message });
                }
              }}
            />

            {/* 趋势图 */}
            <TrendPanel
              points={trendDps}
              wsState={wsState}
              onRemove={(id) => setTrendIds((p) => p.filter((x) => x !== id))}
            />
          </>
        )}
        {/* Dev strip — 骨架演示专用，生产删除 */}
        <div className="mt-2 flex flex-wrap items-center gap-3 rounded-md border border-dashed border-border/50 bg-muted/20 px-3 py-2 text-[11px] text-muted-foreground/60">
          <span>骨架演示：</span>
          <button className="hover:text-muted-foreground hover:underline" onClick={() => { const next = wsState === "connected" ? "reconnecting" : wsState === "reconnecting" ? "disconnected" : "connected"; setWsState(next); }}>
            WS 状态 [{wsState}]
          </button>
          <button className="hover:text-muted-foreground hover:underline" onClick={() => setLatestError((e) => !e)}>
            {latestError ? "✓ /data/latest 失败" : "/data/latest 失败"}
          </button>
          <button className="hover:text-muted-foreground hover:underline" onClick={() => { setSubscriptionError((e) => !e); setSubErrDismissed(false); }}>
            {subscriptionError ? "✓ 订阅失败" : "订阅失败"}
          </button>
        </div>
      </div>

      {/* 点位详情 Drawer */}
      <DatapointDetailDrawer
        datapoint={detail.dp}
        open={detail.open}
        onOpenChange={(o) => setDetail((d) => ({ ...d, open: o }))}
        canControl={canControl}
      />
    </>
  );
}
