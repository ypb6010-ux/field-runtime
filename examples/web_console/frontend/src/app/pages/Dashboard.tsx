import { useState } from "react";
import {
  Activity,
  ArrowUpRight,
  Network,
  Radar,
  Timer,
  GitPullRequestArrow,
  RefreshCw,
} from "lucide-react";
import {
  ResponsiveContainer,
  AreaChart,
  Area,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip as RTooltip,
} from "recharts";
import type { PageKey, Role, StatusTone } from "../types";
import { hasConfigAccess } from "../nav";
import { PageHeader } from "../components/PageHeader";
import { StatusLight } from "../components/StatusLight";
import { DetailDrawer } from "../components/DetailDrawer";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import { Button } from "../components/ui/button";
import { Badge } from "../components/ui/badge";
import { Separator } from "../components/ui/separator";

interface DashboardProps {
  role: Role;
  draftCount: number;
  onNavigate: (page: PageKey) => void;
}

interface Conn {
  id: string;
  name: string;
  driver: string;
  endpoint: string;
  tone: StatusTone;
  label: string;
  points: number;
  pulse?: boolean;
}

const CONNECTIONS: Conn[] = [
  { id: "c1", name: "车间 A · PLC-01", driver: "Modbus TCP", endpoint: "10.20.1.11:502", tone: "success", label: "已连接", points: 128 },
  { id: "c2", name: "车间 A · PLC-02", driver: "Modbus TCP", endpoint: "10.20.1.12:502", tone: "success", label: "已连接", points: 96 },
  { id: "c3", name: "能源站 · 电表网关", driver: "OPC UA", endpoint: "opc.tcp://10.20.2.5:4840", tone: "warning", label: "重连中", points: 64, pulse: true },
  { id: "c4", name: "锅炉房 · 温控", driver: "MQTT", endpoint: "mqtt://10.20.3.8:1883", tone: "success", label: "已连接", points: 42 },
  { id: "c5", name: "仓储 · RFID 读头", driver: "Modbus RTU", endpoint: "/dev/ttyS0", tone: "error", label: "断开", points: 0 },
  { id: "c6", name: "产线 B · 视觉检测", driver: "OPC UA", endpoint: "opc.tcp://10.20.4.2:4840", tone: "disabled", label: "已禁用", points: 0 },
];

type EventModule = "Connection" | "Config" | "Polling" | "System";

const MODULE_STYLE: Record<EventModule, { label: string; cls: string }> = {
  Connection: { label: "连接", cls: "border-primary/30 bg-secondary text-primary" },
  Config: { label: "配置", cls: "border-status-draft-border bg-status-draft-bg text-status-draft" },
  Polling: { label: "轮询", cls: "border-status-warning-border bg-status-warning-bg text-status-warning" },
  System: { label: "系统", cls: "border-border bg-muted text-muted-foreground" },
};

const EVENTS: { id: string; time: string; tone: StatusTone; module: EventModule; msg: string }[] = [
  { id: "e1", time: "10:42:18", tone: "error", module: "Connection", msg: "仓储 · RFID 读头 连接超时（重试 3/3 失败）" },
  { id: "e2", time: "10:41:02", tone: "warning", module: "Connection", msg: "能源站 · 电表网关 触发自动重连" },
  { id: "e3", time: "10:38:55", tone: "draft", module: "Config", msg: "Operator 修改转换规则 R-204，待发布" },
  { id: "e4", time: "10:30:11", tone: "success", module: "Polling", msg: "轮询任务 PLC-01-fast 恢复正常" },
  { id: "e5", time: "10:22:47", tone: "success", module: "System", msg: "配置版本 v37 已发布生效" },
];

const THROUGHPUT = Array.from({ length: 24 }, (_, i) => ({
  t: `${String(i).padStart(2, "0")}:00`,
  in: Math.round(800 + Math.sin(i / 2) * 260 + Math.random() * 120),
  out: Math.round(720 + Math.cos(i / 2.5) * 220 + Math.random() * 100),
}));

function Kpi({
  icon: Icon,
  label,
  value,
  unit,
  tone,
  delta,
  onClick,
}: {
  icon: typeof Network;
  label: string;
  value: string | number;
  unit?: string;
  tone?: StatusTone;
  delta?: string;
  onClick?: () => void;
}) {
  return (
    <Card
      className={`flex-row items-center gap-4 p-4 ${onClick ? "cursor-pointer transition-shadow hover:shadow-md" : ""}`}
      onClick={onClick}
    >
      <div className="flex size-11 shrink-0 items-center justify-center rounded-md bg-secondary text-primary">
        <Icon className="size-5" />
      </div>
      <div className="min-w-0 flex-1">
        <div className="text-xs text-muted-foreground">{label}</div>
        <div className="flex items-baseline gap-1">
          <span className="text-2xl tabular-nums">{value}</span>
          {unit && <span className="text-xs text-muted-foreground">{unit}</span>}
        </div>
      </div>
      {tone && <StatusLight tone={tone} />}
      {delta && (
        <span className="flex items-center gap-0.5 text-xs text-status-success">
          <ArrowUpRight className="size-3" />
          {delta}
        </span>
      )}
    </Card>
  );
}

export function Dashboard({ role, draftCount, onNavigate }: DashboardProps) {
  const [inspect, setInspect] = useState<Conn | null>(null);
  const online = CONNECTIONS.filter((c) => c.tone === "success").length;
  const points = CONNECTIONS.reduce((s, c) => s + c.points, 0);
  const showConfig = hasConfigAccess(role);

  return (
    <>
      <PageHeader
        title="概览"
        en="Dashboard"
        description="采集网关运行总览 · 数据每 5 秒刷新"
        actions={
          <Button variant="outline" size="sm" className="gap-1.5">
            <RefreshCw className="size-3.5" />
            刷新
          </Button>
        }
      />

      <div className="space-y-4 p-6">
        {/* KPI 行 */}
        <div className="grid grid-cols-2 gap-4 lg:grid-cols-4">
          <Kpi icon={Network} label="协议连接" value={`${online}/${CONNECTIONS.length}`} unit="在线" tone={online === CONNECTIONS.length ? "success" : "warning"} onClick={() => onNavigate("protocols")} />
          <Kpi icon={Radar} label="活跃采集点" value={points} unit="点" delta="2.4%" onClick={() => onNavigate("datapoints")} />
          <Kpi icon={Timer} label="轮询任务" value="12" unit="运行中" tone="success" onClick={() => onNavigate("polling")} />
          {showConfig ? (
            <Kpi icon={GitPullRequestArrow} label="未生效项" value={draftCount} unit="待发布" tone={draftCount > 0 ? "draft" : "disabled"} onClick={() => onNavigate("config")} />
          ) : (
            <Kpi icon={Activity} label="实时吞吐" value="1.5k" unit="msg/s" tone="success" onClick={() => onNavigate("live")} />
          )}
        </div>

        <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
          {/* 吞吐趋势 */}
          <Card className="lg:col-span-2">
            <div className="flex items-center justify-between px-6 pt-6">
              <CardTitle>数据吞吐趋势（近 24h）</CardTitle>
              <div className="flex items-center gap-3 text-xs text-muted-foreground">
                <span className="flex items-center gap-1.5"><span className="size-2 rounded-full bg-chart-1" />采集</span>
                <span className="flex items-center gap-1.5"><span className="size-2 rounded-full bg-chart-2" />下发</span>
              </div>
            </div>
            <CardContent>
              <div className="h-64 w-full">
                <ResponsiveContainer width="100%" height="100%">
                  <AreaChart data={THROUGHPUT} margin={{ top: 4, right: 8, left: -16, bottom: 0 }}>
                    <defs>
                      <linearGradient id="gIn" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="0%" stopColor="var(--chart-1)" stopOpacity={0.35} />
                        <stop offset="100%" stopColor="var(--chart-1)" stopOpacity={0} />
                      </linearGradient>
                      <linearGradient id="gOut" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="0%" stopColor="var(--chart-2)" stopOpacity={0.3} />
                        <stop offset="100%" stopColor="var(--chart-2)" stopOpacity={0} />
                      </linearGradient>
                    </defs>
                    <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" vertical={false} />
                    <XAxis dataKey="t" tick={{ fontSize: 11, fill: "var(--muted-foreground)" }} interval={3} axisLine={false} tickLine={false} />
                    <YAxis tick={{ fontSize: 11, fill: "var(--muted-foreground)" }} axisLine={false} tickLine={false} />
                    <RTooltip
                      contentStyle={{ fontSize: 12, borderRadius: 8, border: "1px solid var(--border)" }}
                      labelStyle={{ color: "var(--muted-foreground)" }}
                    />
                    <Area type="monotone" dataKey="in" stroke="var(--chart-1)" strokeWidth={2} fill="url(#gIn)" name="采集" />
                    <Area type="monotone" dataKey="out" stroke="var(--chart-2)" strokeWidth={2} fill="url(#gOut)" name="下发" />
                  </AreaChart>
                </ResponsiveContainer>
              </div>
            </CardContent>
          </Card>

          {/* 最近事件 */}
          <Card>
            <div className="flex items-center justify-between px-6 pt-6">
              <CardTitle>最近事件</CardTitle>
              <Button variant="ghost" size="sm" className="h-7 text-xs" onClick={() => onNavigate("live")}>
                查看全部
              </Button>
            </div>
            <CardContent className="space-y-0">
              {EVENTS.map((e, i) => (
                <div key={e.id}>
                  {i > 0 && <Separator />}
                  <div className="flex items-start gap-2.5 py-2.5">
                    <StatusLight tone={e.tone} className="mt-1" />
                    <div className="min-w-0 flex-1">
                      <div className="flex items-center justify-between gap-2">
                        <Badge variant="outline" className={`px-1.5 py-0 ${MODULE_STYLE[e.module].cls}`}>
                          {MODULE_STYLE[e.module].label} · {e.module}
                        </Badge>
                        <span className="font-mono text-[11px] text-muted-foreground">{e.time}</span>
                      </div>
                      <p className="text-sm leading-snug">{e.msg}</p>
                    </div>
                  </div>
                </div>
              ))}
            </CardContent>
          </Card>
        </div>

        {/* 连接状态墙 */}
        <Card>
          <div className="flex items-center justify-between px-6 pt-6">
            <CardTitle>连接状态总览</CardTitle>
            <Button variant="ghost" size="sm" className="h-7 text-xs" onClick={() => onNavigate("protocols")}>
              管理协议
            </Button>
          </div>
          <CardContent>
            <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
              {CONNECTIONS.map((c) => (
                <button
                  key={c.id}
                  type="button"
                  onClick={() => setInspect(c)}
                  className="flex items-center justify-between gap-3 rounded-md border border-border bg-card px-3 py-2.5 text-left transition-colors hover:border-primary/40 hover:bg-secondary/40"
                >
                  <div className="flex min-w-0 items-center gap-2.5">
                    <StatusLight tone={c.tone} pulse={c.pulse} />
                    <div className="min-w-0">
                      <div className="truncate text-sm">{c.name}</div>
                      <div className="truncate font-mono text-[11px] text-muted-foreground">{c.endpoint}</div>
                    </div>
                  </div>
                  <Badge variant="outline" className="shrink-0">{c.driver}</Badge>
                </button>
              ))}
            </div>
          </CardContent>
        </Card>
      </div>

      {/* 右侧详情抽屉模式演示 */}
      <DetailDrawer
        open={!!inspect}
        onOpenChange={(o) => !o && setInspect(null)}
        title={inspect?.name}
        description={inspect ? `${inspect.driver} · ${inspect.endpoint}` : undefined}
        footer={
          <>
            <Button variant="outline" onClick={() => setInspect(null)}>关闭</Button>
            <Button onClick={() => { onNavigate("protocols"); setInspect(null); }}>编辑连接</Button>
          </>
        }
      >
        {inspect && (
          <div className="space-y-4">
            <div className="flex items-center gap-2">
              <span className="text-sm text-muted-foreground">当前状态</span>
              <StatusLight tone={inspect.tone} label={inspect.label} pulse={inspect.pulse} />
            </div>
            <Separator />
            <dl className="grid grid-cols-2 gap-y-3 text-sm">
              <dt className="text-muted-foreground">驱动</dt>
              <dd>{inspect.driver}</dd>
              <dt className="text-muted-foreground">端点</dt>
              <dd className="font-mono text-xs">{inspect.endpoint}</dd>
              <dt className="text-muted-foreground">采集点数</dt>
              <dd className="tabular-nums">{inspect.points}</dd>
              <dt className="text-muted-foreground">最近采样</dt>
              <dd>{inspect.tone === "success" ? "1 秒前" : "—"}</dd>
            </dl>
            <Separator />
            <p className="text-xs text-muted-foreground">
              这是右侧详情面板的统一模式：从列表 / 状态墙点击行，在不离开当前上下文的情况下查看或进入编辑。
            </p>
          </div>
        )}
      </DetailDrawer>
    </>
  );
}
