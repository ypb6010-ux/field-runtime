import { useEffect, useState } from "react";
import {
  Activity,
  GitPullRequestArrow,
  Network,
  Radar,
  RefreshCw,
  ShieldAlert,
} from "lucide-react";
import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip as ChartTooltip,
  XAxis,
  YAxis,
} from "recharts";
import { apiGet } from "../api";
import { hasConfigAccess } from "../nav";
import type { PageKey, Role, StatusTone } from "../types";
import { DetailDrawer } from "../components/DetailDrawer";
import { PageHeader } from "../components/PageHeader";
import { StatusLight } from "../components/StatusLight";
import { Badge } from "../components/ui/badge";
import { Button } from "../components/ui/button";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import { Separator } from "../components/ui/separator";

interface DashboardProps {
  role: Role;
  draftCount: number;
  onNavigate: (page: PageKey) => void;
}

interface RuntimeTransport {
  id: string;
  kind: string;
  state: string;
}

interface RuntimePoint {
  id: string;
  quality: string;
}

interface CatalogPoint {
  transport_id: string;
}

interface Connection {
  id: string;
  driver: string;
  tone: StatusTone;
  label: string;
  points: number;
  pulse?: boolean;
}

interface Sample {
  time: string;
  online: number;
  healthyPoints: number;
}

const STATE_TONE: Record<string, { tone: StatusTone; label: string }> = {
  connected: { tone: "success", label: "已连接" },
  connecting: { tone: "warning", label: "连接中" },
  error: { tone: "error", label: "错误" },
  disconnected: { tone: "disabled", label: "未连接" },
};

const KIND_LABEL: Record<string, string> = {
  modbus_tcp_client: "Modbus TCP",
  modbus_rtu: "Modbus RTU",
  opc_ua_client: "OPC UA",
  s7_client: "Siemens S7",
};

function Kpi({
  icon: Icon,
  label,
  value,
  unit,
  tone,
  onClick,
}: {
  icon: typeof Network;
  label: string;
  value: string | number;
  unit?: string;
  tone?: StatusTone;
  onClick?: () => void;
}) {
  return (
    <Card
      className={`flex-row items-center gap-4 rounded-lg p-4 shadow-sm ${
        onClick ? "cursor-pointer transition-shadow hover:shadow-md" : ""
      }`}
      onClick={onClick}
    >
      <div className="flex size-11 shrink-0 items-center justify-center rounded-lg bg-secondary text-primary">
        <Icon className="size-5" />
      </div>
      <div className="min-w-0 flex-1">
        <div className="text-xs text-muted-foreground">{label}</div>
        <div className="flex items-baseline gap-1">
          <span className="text-2xl font-semibold tabular-nums">{value}</span>
          {unit && <span className="text-xs text-muted-foreground">{unit}</span>}
        </div>
      </div>
      {tone && <StatusLight tone={tone} />}
    </Card>
  );
}

export function Dashboard({ role, draftCount, onNavigate }: DashboardProps) {
  const [connections, setConnections] = useState<Connection[]>([]);
  const [points, setPoints] = useState<RuntimePoint[]>([]);
  const [samples, setSamples] = useState<Sample[]>([]);
  const [inspect, setInspect] = useState<Connection | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [refreshing, setRefreshing] = useState(false);

  async function load() {
    setRefreshing(true);
    try {
      const [runtimeTransports, runtimePoints, catalog] = await Promise.all([
        apiGet<RuntimeTransport[]>("/runtime/transports"),
        apiGet<RuntimePoint[]>("/data/latest"),
        apiGet<CatalogPoint[]>("/data/catalog"),
      ]);
      const pointCounts = new Map<string, number>();
      catalog.forEach((point) => {
        pointCounts.set(
          point.transport_id,
          (pointCounts.get(point.transport_id) ?? 0) + 1,
        );
      });
      const mapped = runtimeTransports.map((transport) => {
        const state = STATE_TONE[transport.state] ?? {
          tone: "disabled" as StatusTone,
          label: transport.state,
        };
        return {
          id: transport.id,
          driver: KIND_LABEL[transport.kind] ?? transport.kind,
          tone: state.tone,
          label: state.label,
          points: pointCounts.get(transport.id) ?? 0,
          pulse: transport.state === "connecting",
        };
      });
      setConnections(mapped);
      setPoints(runtimePoints);
      const healthyPoints = runtimePoints.filter(
        (point) => point.quality === "Ok" || point.quality === "Good",
      ).length;
      setSamples((current) => [
        ...current,
        {
          time: new Date().toLocaleTimeString("zh-CN", { hour12: false }),
          online: mapped.filter((connection) => connection.tone === "success").length,
          healthyPoints,
        },
      ].slice(-60));
      setError(null);
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : "运行状态加载失败");
    } finally {
      setRefreshing(false);
    }
  }

  useEffect(() => {
    load();
    const timer = window.setInterval(load, 5000);
    return () => window.clearInterval(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const online = connections.filter((connection) => connection.tone === "success").length;
  const healthy = points.filter(
    (point) => point.quality === "Ok" || point.quality === "Good",
  ).length;
  const issues = [
    ...connections
      .filter((connection) => connection.tone !== "success")
      .map((connection) => ({
        id: `transport-${connection.id}`,
        title: connection.id,
        detail: connection.label,
        tone: connection.tone,
      })),
    ...points
      .filter((point) => point.quality !== "Ok" && point.quality !== "Good")
      .slice(0, 10)
      .map((point) => ({
        id: `point-${point.id}`,
        title: point.id,
        detail: `质量：${point.quality}`,
        tone: point.quality === "Error" ? "error" as StatusTone : "warning" as StatusTone,
      })),
  ];
  const canConfigure = hasConfigAccess(role);

  return (
    <>
      <PageHeader
        title="概览"
        en="Dashboard"
        description="运行时连接与点位健康状态 · 每 5 秒刷新"
        actions={
          <Button variant="outline" size="sm" onClick={load} disabled={refreshing}>
            <RefreshCw className={`size-3.5 ${refreshing ? "animate-spin" : ""}`} />
            刷新
          </Button>
        }
      />

      <div className="space-y-4 p-6">
        {error && (
          <div className="flex items-center gap-2 rounded-lg border border-red-200 bg-red-50 p-3 text-sm text-red-700">
            <ShieldAlert className="size-4" />{error}
          </div>
        )}

        <div className="grid grid-cols-2 gap-4 lg:grid-cols-4">
          <Kpi
            icon={Network}
            label="协议连接"
            value={`${online}/${connections.length}`}
            unit="在线"
            tone={connections.length > 0 && online === connections.length ? "success" : "warning"}
            onClick={canConfigure ? () => onNavigate("protocols") : undefined}
          />
          <Kpi
            icon={Radar}
            label="运行时点位"
            value={points.length}
            unit="点"
            tone={points.length > 0 ? "success" : "disabled"}
            onClick={() => onNavigate("live")}
          />
          <Kpi
            icon={Activity}
            label="质量正常"
            value={`${healthy}/${points.length}`}
            unit="点"
            tone={points.length > 0 && healthy === points.length ? "success" : "warning"}
            onClick={() => onNavigate("live")}
          />
          {canConfigure ? (
            <Kpi
              icon={GitPullRequestArrow}
              label="草稿状态"
              value={draftCount > 0 ? "待发布" : "已同步"}
              tone={draftCount > 0 ? "draft" : "success"}
              onClick={() => onNavigate("config")}
            />
          ) : (
            <Kpi
              icon={ShieldAlert}
              label="当前问题"
              value={issues.length}
              unit="项"
              tone={issues.length > 0 ? "warning" : "success"}
              onClick={() => onNavigate("live")}
            />
          )}
        </div>

        <div className="grid gap-4 lg:grid-cols-[minmax(0,1.5fr)_minmax(320px,0.7fr)]">
          <Card className="rounded-lg">
            <div className="px-6 pt-6">
              <CardTitle>本次页面会话健康趋势</CardTitle>
            </div>
            <CardContent>
              <div className="h-64">
                {samples.length < 2 ? (
                  <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
                    正在积累真实状态样本…
                  </div>
                ) : (
                  <ResponsiveContainer width="100%" height="100%">
                    <AreaChart data={samples}>
                      <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" vertical={false} />
                      <XAxis dataKey="time" tick={{ fontSize: 10 }} interval="preserveStartEnd" />
                      <YAxis allowDecimals={false} tick={{ fontSize: 10 }} />
                      <ChartTooltip />
                      <Area type="monotone" dataKey="healthyPoints" name="质量正常点位" stroke="var(--chart-1)" fill="var(--chart-1)" fillOpacity={0.18} />
                      <Area type="monotone" dataKey="online" name="在线连接" stroke="var(--chart-2)" fill="var(--chart-2)" fillOpacity={0.12} />
                    </AreaChart>
                  </ResponsiveContainer>
                )}
              </div>
            </CardContent>
          </Card>

          <Card className="rounded-lg">
            <div className="flex items-center justify-between px-6 pt-6">
              <CardTitle>当前问题</CardTitle>
              <Badge variant="outline">{issues.length}</Badge>
            </div>
            <CardContent className="mt-3 max-h-64 space-y-0 overflow-auto">
              {issues.map((issue, index) => (
                <div key={issue.id}>
                  {index > 0 && <Separator />}
                  <div className="flex items-start gap-2 py-2.5">
                    <StatusLight tone={issue.tone} className="mt-1" />
                    <div>
                      <div className="text-sm font-medium">{issue.title}</div>
                      <div className="text-xs text-muted-foreground">{issue.detail}</div>
                    </div>
                  </div>
                </div>
              ))}
              {issues.length === 0 && (
                <div className="py-12 text-center text-sm text-muted-foreground">
                  当前未发现连接或质量问题
                </div>
              )}
            </CardContent>
          </Card>
        </div>

        <Card className="rounded-lg">
          <div className="flex items-center justify-between px-6 pt-6">
            <CardTitle>连接状态</CardTitle>
            {canConfigure && (
              <Button variant="ghost" size="sm" onClick={() => onNavigate("protocols")}>管理协议</Button>
            )}
          </div>
          <CardContent>
            <div className="grid gap-3 md:grid-cols-2 xl:grid-cols-3">
              {connections.map((connection) => (
                <button
                  key={connection.id}
                  type="button"
                  onClick={() => setInspect(connection)}
                  className="flex items-center justify-between rounded-lg border px-3 py-3 text-left transition-colors hover:border-primary/40 hover:bg-secondary/30"
                >
                  <div className="flex min-w-0 items-center gap-2.5">
                    <StatusLight tone={connection.tone} pulse={connection.pulse} />
                    <div className="min-w-0">
                      <div className="truncate text-sm font-medium">{connection.id}</div>
                      <div className="text-xs text-muted-foreground">{connection.points} 个点位</div>
                    </div>
                  </div>
                  <Badge variant="outline">{connection.driver}</Badge>
                </button>
              ))}
              {connections.length === 0 && (
                <div className="col-span-full py-10 text-center text-sm text-muted-foreground">
                  运行时暂无协议连接
                </div>
              )}
            </div>
          </CardContent>
        </Card>
      </div>

      <DetailDrawer
        open={!!inspect}
        onOpenChange={(open) => !open && setInspect(null)}
        title={inspect?.id}
        description={inspect?.driver}
        footer={<Button variant="outline" onClick={() => setInspect(null)}>关闭</Button>}
      >
        {inspect && (
          <dl className="grid grid-cols-[7rem_1fr] gap-y-3 text-sm">
            <dt className="text-muted-foreground">运行状态</dt>
            <dd><StatusLight tone={inspect.tone} label={inspect.label} pulse={inspect.pulse} /></dd>
            <dt className="text-muted-foreground">协议驱动</dt>
            <dd>{inspect.driver}</dd>
            <dt className="text-muted-foreground">活动点位数</dt>
            <dd>{inspect.points}</dd>
          </dl>
        )}
      </DetailDrawer>
    </>
  );
}
