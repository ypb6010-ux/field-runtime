import { useEffect, useMemo, useRef, useState } from "react";
import { AlertCircle, ChevronLeft, ChevronRight, Pause, Play, RefreshCw, RotateCcw } from "lucide-react";
import { toast } from "sonner";
import {
  apiGet,
  connectStream,
  type StreamConnection,
  type WsSnapshot,
} from "../api";
import type {
  Datapoint,
  DataType,
  DpStatus,
  Quality,
  Trend,
  TrendPoint,
  WsState,
} from "../live";
import { STALE_THRESHOLD_S } from "../live";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { DatapointDetailDrawer } from "../components/live/DatapointDetailDrawer";
import { LatestDataErrorState, LiveEmptyState } from "../components/live/LiveEmptyError";
import {
  EMPTY_LIVE_FILTERS,
  LiveFilterBar,
  type LiveFilters,
} from "../components/live/LiveFilterBar";
import {
  LiveStatusBar,
  PausedBanner,
  WsStatusBanner,
} from "../components/live/LiveStatusBar";
import { RealTimeDatapointTable } from "../components/live/RealTimeDatapointTable";
import { TrendPanel } from "../components/live/TrendPanel";
import type { Role } from "../types";

interface RawDp {
  id: string;
  value: unknown;
  quality: string;
  ts: number;
}

interface CatalogPoint {
  id: string;
  transport_id: string;
  reg_table: string;
  addr: string;
  type: string;
}

const LIVE_PAGE_SIZE = 100;

function qualityOf(value: string): Quality {
  if (value === "Ok" || value === "Good") return "Good";
  if (value === "Error" || value === "Bad") return "Bad";
  return "Uncertain";
}

function toDatapoint(
  previous: Datapoint | undefined,
  snapshot: RawDp,
  catalog?: CatalogPoint,
): Datapoint {
  const isNumber = typeof snapshot.value === "number";
  const dataType: DataType =
    typeof snapshot.value === "boolean"
      ? "boolean"
      : isNumber
        ? "number"
        : "string";
  const quality = qualityOf(snapshot.quality);
  const value =
    snapshot.value === null || snapshot.value === undefined
      ? "—"
      : snapshot.value as number | boolean | string;
  const previousValue = previous?.value;
  const trend: Trend =
    typeof value === "number" && typeof previousValue === "number"
      ? value > previousValue
        ? "up"
        : value < previousValue
          ? "down"
          : "flat"
      : "flat";
  const ageSeconds = Math.max(
    0,
    Math.floor((Date.now() - (snapshot.ts || Date.now())) / 1000),
  );
  const status: DpStatus =
    quality === "Bad"
      ? "error"
      : ageSeconds > STALE_THRESHOLD_S
        ? "stale"
        : "normal";
  return {
    id: snapshot.id,
    name: snapshot.id,
    address: catalog ? `${catalog.reg_table} ${catalog.addr}` : previous?.address ?? "—",
    transportId: catalog?.transport_id ?? previous?.transportId ?? "",
    transportName: catalog?.transport_id ?? previous?.transportName ?? "运行时",
    dataType,
    unit: previous?.unit ?? "",
    quality,
    status,
    value,
    prevValue: previousValue,
    trend,
    timestamp: new Date(snapshot.ts || Date.now()).toLocaleTimeString(
      "zh-CN",
      { hour12: false },
    ),
    ageSeconds,
    tags: previous?.tags ?? [],
    justUpdated: previousValue !== undefined && previousValue !== value,
  };
}

export function Live({ role }: { role: Role }) {
  const [datapoints, setDatapoints] = useState<Datapoint[]>([]);
  const [catalog, setCatalog] = useState<CatalogPoint[]>([]);
  const [latestError, setLatestError] = useState<string | null>(null);
  const [filters, setFilters] = useState<LiveFilters>(EMPTY_LIVE_FILTERS);
  const [wsState, setWsState] = useState<WsState>("reconnecting");
  const [paused, setPaused] = useState(false);
  const [updateRate, setUpdateRate] = useState(0);
  const [page, setPage] = useState(0);
  const [trendIds, setTrendIds] = useState<string[]>([]);
  const [trendHistory, setTrendHistory] = useState<Record<string, TrendPoint[]>>({});
  const [detail, setDetail] = useState<{ open: boolean; dp: Datapoint | null }>({
    open: false,
    dp: null,
  });
  const [lastUpdate, setLastUpdate] = useState("—");
  const connectionRef = useRef<StreamConnection | null>(null);
  const pausedRef = useRef(paused);
  const catalogRef = useRef(catalog);
  const trendIdsRef = useRef(trendIds);

  useEffect(() => {
    pausedRef.current = paused;
  }, [paused]);
  useEffect(() => {
    catalogRef.current = catalog;
  }, [catalog]);
  useEffect(() => {
    trendIdsRef.current = trendIds;
  }, [trendIds]);

  function appendTrend(snapshots: RawDp[]) {
    if (trendIdsRef.current.length === 0) return;
    setTrendHistory((current) => {
      const next = { ...current };
      let changed = false;
      for (const snapshot of snapshots) {
        if (
          typeof snapshot.value !== "number"
          || !trendIdsRef.current.includes(snapshot.id)
        ) {
          continue;
        }
        const points = [...(next[snapshot.id] ?? [])];
        const timestamp = snapshot.ts || Date.now();
        if (points[points.length - 1]?.ts === timestamp) continue;
        points.push({
          ts: timestamp,
          t: new Date(timestamp).toLocaleTimeString("zh-CN", { hour12: false }),
          value: snapshot.value,
        });
        next[snapshot.id] = points.slice(-3600);
        changed = true;
      }
      return changed ? next : current;
    });
  }

  async function refreshLatest() {
    try {
      const [latest, nextCatalog] = await Promise.all([
        apiGet<RawDp[]>("/data/latest"),
        apiGet<CatalogPoint[]>("/data/catalog"),
      ]);
      setCatalog(nextCatalog);
      const catalogById = new Map(nextCatalog.map((point) => [point.id, point]));
      setDatapoints((previous) => {
        const previousById = new Map(previous.map((point) => [point.id, point]));
        return latest.map((snapshot) =>
          toDatapoint(
            previousById.get(snapshot.id),
            snapshot,
            catalogById.get(snapshot.id),
          ),
        );
      });
      appendTrend(latest);
      setLatestError(null);
      setLastUpdate(new Date().toLocaleTimeString("zh-CN", { hour12: false }));
    } catch (error) {
      setLatestError(error instanceof Error ? error.message : "最新数据加载失败");
    }
  }

  useEffect(() => {
    refreshLatest();
    const connection = connectStream({
      topics: ["dp/*"],
      onState: (state) => {
        setWsState(
          state === "connected"
            ? "connected"
            : state === "disconnected"
              ? "disconnected"
              : "reconnecting",
        );
      },
      onSnapshot: (snapshot: WsSnapshot) => {
        if (pausedRef.current) return;
        const incoming = (snapshot.datapoints ?? []) as RawDp[];
        if (incoming.length === 0) return;
        const catalogById = new Map(
          catalogRef.current.map((point) => [point.id, point]),
        );
        setDatapoints((previous) => {
          const next = new Map(previous.map((point) => [point.id, point]));
          for (const item of incoming) {
            next.set(
              item.id,
              toDatapoint(next.get(item.id), item, catalogById.get(item.id)),
            );
          }
          return [...next.values()];
        });
        appendTrend(incoming);
        setLastUpdate(new Date().toLocaleTimeString("zh-CN", { hour12: false }));
        setUpdateRate(incoming.length);
      },
    });
    connectionRef.current = connection;
    const ageTimer = window.setInterval(() => {
      setDatapoints((previous) =>
        previous.map((point) => {
          const ageSeconds = point.ageSeconds + 1;
          return {
            ...point,
            ageSeconds,
            justUpdated: false,
            status:
              point.quality === "Bad"
                ? "error"
                : ageSeconds > STALE_THRESHOLD_S
                  ? "stale"
                  : "normal",
          };
        }),
      );
    }, 1000);
    return () => {
      connection.close();
      connectionRef.current = null;
      window.clearInterval(ageTimer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const staleCount = datapoints.filter(
    (point) => point.ageSeconds > STALE_THRESHOLD_S,
  ).length;
  const transports = useMemo(
    () =>
      Array.from(
        new Map(
          datapoints
            .filter((point) => point.transportId)
            .map((point) => [
              point.transportId,
              { id: point.transportId, name: point.transportName },
            ] as const),
        ).values(),
      ),
    [datapoints],
  );
  const filtered = useMemo(() => {
    const keyword = filters.keyword.trim().toLowerCase();
    return datapoints.filter((point) => {
      if (filters.transportId !== "all" && point.transportId !== filters.transportId) return false;
      if (filters.status !== "all" && point.status !== filters.status) return false;
      if (filters.dataType !== "all" && point.dataType !== filters.dataType) return false;
      if (filters.quality !== "all" && point.quality !== filters.quality) return false;
      if (filters.onlyChanging && point.trend === "flat") return false;
      return !keyword
        || `${point.id} ${point.name} ${point.address} ${point.tags.join(" ")}`
          .toLowerCase()
          .includes(keyword);
    });
  }, [datapoints, filters]);
  const isFiltering =
    JSON.stringify(filters) !== JSON.stringify(EMPTY_LIVE_FILTERS);
  const pageCount = Math.max(1, Math.ceil(filtered.length / LIVE_PAGE_SIZE));
  const visiblePage = Math.min(page, pageCount - 1);
  const visibleRows = filtered.slice(
    visiblePage * LIVE_PAGE_SIZE,
    (visiblePage + 1) * LIVE_PAGE_SIZE,
  );
  const trendPoints = datapoints.filter((point) => trendIds.includes(point.id));
  const detailPoint =
    datapoints.find((point) => point.id === detail.dp?.id) ?? detail.dp;

  function addToTrend(point: Datapoint) {
    if (point.dataType !== "number") {
      toast.message("仅数值点位支持趋势图");
      return;
    }
    const current = trendIdsRef.current;
    if (current.includes(point.id)) return;
    if (current.length >= 5) {
      toast.error("趋势图最多同时展示 5 个点位");
      return;
    }

    const next = [...current, point.id];
    trendIdsRef.current = next;
    setTrendIds(next);
    if (typeof point.value === "number") {
      const now = Date.now();
      setTrendHistory((history) => ({
        ...history,
        [point.id]: [
          ...(history[point.id] ?? []),
          {
            ts: now,
            t: new Date(now).toLocaleTimeString("zh-CN", { hour12: false }),
            value: point.value as number,
          },
        ].slice(-3600),
      }));
    }
  }

  return (
    <>
      <PageHeader
        title="实时监控"
        en="Live"
        description="通过 WebSocket 展示运行时点位值、质量与真实趋势。"
        actions={
          <>
            <Button variant="ghost" size="sm" onClick={refreshLatest}>
              <RefreshCw className="size-3.5" />刷新快照
            </Button>
            {wsState !== "connected" ? (
              <Button
                variant="outline"
                size="sm"
                onClick={() => connectionRef.current?.reconnect()}
              >
                <RotateCcw className="size-3.5" />立即重连
              </Button>
            ) : (
              <Button
                variant="outline"
                size="sm"
                onClick={() => setPaused((current) => !current)}
              >
                {paused ? <Play className="size-3.5" /> : <Pause className="size-3.5" />}
                {paused ? "恢复更新" : "暂停更新"}
              </Button>
            )}
          </>
        }
      />

      <div className="flex flex-col gap-4 p-6">
        <WsStatusBanner wsState={wsState} />
        {paused && wsState === "connected" && <PausedBanner />}
        <LiveStatusBar
          wsState={wsState}
          lastUpdate={lastUpdate}
          subscriptions={datapoints.length}
          updateRate={paused ? 0 : updateRate}
          staleCount={staleCount}
        />
        <LiveFilterBar
          filters={filters}
          transports={transports}
          onChange={(next) => {
            setFilters(next);
            setPage(0);
          }}
          onReset={() => {
            setFilters(EMPTY_LIVE_FILTERS);
            setPage(0);
          }}
        />

        {latestError ? (
          <LatestDataErrorState
            onRetry={refreshLatest}
          />
        ) : filtered.length === 0 ? (
          <LiveEmptyState
            isFiltering={isFiltering}
            canConfig={role === "admin"}
            onReset={() => {
              setFilters(EMPTY_LIVE_FILTERS);
              setPage(0);
            }}
          />
        ) : (
          <>
            <div className="flex flex-wrap items-center justify-between gap-2 text-xs text-muted-foreground">
              <div className="flex items-center gap-2">
                <span>{filtered.length} 个点位{isFiltering ? "（已筛选）" : ""}</span>
                {staleCount > 0 && (
                  <span className="flex items-center gap-1 text-status-warning">
                    <AlertCircle className="size-3" />{staleCount} 个点位已过期
                  </span>
                )}
              </div>
              {pageCount > 1 && (
                <div className="flex items-center gap-1">
                  <span className="mr-1">
                    {visiblePage * LIVE_PAGE_SIZE + 1}–
                    {Math.min(filtered.length, (visiblePage + 1) * LIVE_PAGE_SIZE)}
                    {" / "}{filtered.length}
                  </span>
                  <Button
                    variant="outline"
                    size="icon"
                    className="size-7"
                    aria-label="上一页"
                    disabled={visiblePage === 0}
                    onClick={() => setPage(Math.max(0, visiblePage - 1))}
                  >
                    <ChevronLeft className="size-3.5" />
                  </Button>
                  <Button
                    variant="outline"
                    size="icon"
                    className="size-7"
                    aria-label="下一页"
                    disabled={visiblePage >= pageCount - 1}
                    onClick={() => setPage(Math.min(pageCount - 1, visiblePage + 1))}
                  >
                    <ChevronRight className="size-3.5" />
                  </Button>
                </div>
              )}
            </div>
            <RealTimeDatapointTable
              rows={visibleRows}
              wsState={wsState}
              selectedId={detailPoint?.id}
              onRowClick={(point) => setDetail({ open: true, dp: point })}
              onTrend={addToTrend}
              onReadNow={(point, result) => {
                if (!result.ok) {
                  toast.error(`${point.name} 刷新失败`, { description: result.message });
                  return;
                }
                refreshLatest();
                toast.success(`${point.name} 已刷新 · ${result.latencyMs}ms`);
              }}
            />
            <TrendPanel
              points={trendPoints}
              history={trendHistory}
              wsState={wsState}
              onRemove={(id) => {
                const next = trendIdsRef.current.filter((item) => item !== id);
                trendIdsRef.current = next;
                setTrendIds(next);
                setTrendHistory((history) => {
                  const next = { ...history };
                  delete next[id];
                  return next;
                });
              }}
            />
          </>
        )}
      </div>

      <DatapointDetailDrawer
        datapoint={detailPoint}
        open={detail.open}
        onOpenChange={(open) => setDetail((current) => ({ ...current, open }))}
      />
    </>
  );
}
