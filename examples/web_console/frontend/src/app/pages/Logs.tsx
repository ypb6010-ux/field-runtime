import { useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { Download, Eye, RefreshCw, Trash2 } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Badge } from "../components/ui/badge";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../components/ui/select";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";
import {
  Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription,
} from "../components/ui/sheet";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/ui/tabs";
import { Label } from "../components/ui/label";

type LogLevel = "info" | "warning" | "error";
type Module = "system" | "transport" | "polling" | "conversion" | "auth" | "config";
type AuditResult = "success" | "failed" | "denied";

interface SystemEvent {
  id: string;
  time: string;
  level: LogLevel;
  module: Module;
  type: string;
  message: string;
  source: string;
  requestId: string;
  traceId: string;
  object: string;
  payload: string;
}

interface AuditLog {
  id: string;
  time: string;
  user: string;
  action: string;
  resource: string;
  result: AuditResult;
  ip: string;
  detail: string;
  requestId: string;
  traceId: string;
  payload: string;
}

type Detail =
  | { kind: "event"; row: SystemEvent }
  | { kind: "audit"; row: AuditLog };

const STATUS_META: Record<LogLevel | AuditResult, { label: string; cls: string }> = {
  info: { label: "info", cls: "bg-sky-50 text-sky-700 border-sky-200" },
  warning: { label: "warning", cls: "bg-amber-50 text-amber-700 border-amber-200" },
  error: { label: "error", cls: "bg-red-50 text-red-700 border-red-200" },
  success: { label: "success", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  failed: { label: "failed", cls: "bg-red-50 text-red-700 border-red-200" },
  denied: { label: "denied", cls: "bg-amber-50 text-amber-700 border-amber-200" },
};

const SYSTEM_EVENTS: SystemEvent[] = [
  { id: "evt-1", time: "2026-06-18 10:22:04", level: "info", module: "polling", type: "poll.tick", message: "锅炉温压快采完成 28 个点位采集", source: "poller-a", requestId: "req-7a21", traceId: "tr-10091", object: "poll-1", payload: "{ \"durationMs\": 42, \"points\": 28, \"quality\": \"good\" }" },
  { id: "evt-2", time: "2026-06-18 10:21:55", level: "error", module: "conversion", type: "target.write_failed", message: "电机转速限幅写入 S7 目标超时", source: "conversion-worker-2", requestId: "req-7a02", traceId: "tr-10077", object: "rule-204", payload: "{ \"target\": \"DB1.DBD20\", \"timeoutMs\": 3000 }" },
  { id: "evt-3", time: "2026-06-18 10:20:31", level: "warning", module: "transport", type: "latency.high", message: "OPC-UA 产线B 延迟超过阈值", source: "opcua-client-b", requestId: "req-69ef", traceId: "tr-10031", object: "transport-opc-b", payload: "{ \"latencyMs\": 840, \"thresholdMs\": 500 }" },
  { id: "evt-4", time: "2026-06-18 10:18:44", level: "info", module: "config", type: "config.validated", message: "草稿配置校验通过，warnings: 1", source: "config-service", requestId: "req-67c1", traceId: "tr-09988", object: "draft-v38", payload: "{ \"checked\": 128, \"warnings\": 1, \"errors\": 0 }" },
  { id: "evt-5", time: "2026-06-18 10:15:12", level: "info", module: "system", type: "runtime.heartbeat", message: "系统心跳正常，WS 连接 18 个", source: "runtime", requestId: "req-6500", traceId: "tr-09912", object: "runtime-main", payload: "{ \"wsClients\": 18, \"uptimeSec\": 86320 }" },
  { id: "evt-6", time: "2026-06-18 10:12:06", level: "warning", module: "auth", type: "login.retry", message: "用户连续登录失败 2 次", source: "auth-service", requestId: "req-6331", traceId: "tr-09881", object: "user-li", payload: "{ \"attempts\": 2, \"ip\": \"10.12.4.33\" }" },
];

const AUDIT_LOGS: AuditLog[] = [
  { id: "aud-1", time: "2026-06-18 10:19:02", user: "张工 / Admin", action: "Validate", resource: "draft config v38", result: "success", ip: "10.12.4.21", detail: "校验草稿配置，checked items: 128", requestId: "req-a881", traceId: "tr-a1001", payload: "{ \"warnings\": 1, \"errors\": 0 }" },
  { id: "aud-2", time: "2026-06-18 10:17:44", user: "张工 / Admin", action: "Update", resource: "conversion rule R-204", result: "success", ip: "10.12.4.21", detail: "调整字段映射和 fallback value", requestId: "req-a840", traceId: "tr-a0982", payload: "{ \"field\": \"transform.expression\", \"after\": \"clamp(value,0,1800)\" }" },
  { id: "aud-3", time: "2026-06-18 10:12:09", user: "李工 / Operator", action: "Login", resource: "web_console", result: "failed", ip: "10.12.4.33", detail: "密码错误", requestId: "req-a771", traceId: "tr-a0911", payload: "{ \"reason\": \"bad_credentials\" }" },
  { id: "aud-4", time: "2026-06-18 10:05:18", user: "张工 / Admin", action: "Disable", resource: "polling task 循环计数低频采集", result: "success", ip: "10.12.4.21", detail: "维护窗口暂停任务", requestId: "req-a620", traceId: "tr-a0812", payload: "{ \"task\": \"poll-5\", \"enabled\": false }" },
  { id: "aud-5", time: "2026-06-18 09:58:43", user: "王工 / Viewer", action: "Delete", resource: "datapoint 急停信号", result: "denied", ip: "10.12.4.55", detail: "按钮级权限不足，后端拒绝删除", requestId: "req-a509", traceId: "tr-a0705", payload: "{ \"permission\": \"datapoints.delete\" }" },
  { id: "aud-6", time: "2026-06-18 09:44:20", user: "张工 / Admin", action: "Export", resource: "system events", result: "success", ip: "10.12.4.21", detail: "导出最近 24 小时系统事件", requestId: "req-a410", traceId: "tr-a0601", payload: "{ \"range\": \"24h\", \"rows\": 4201 }" },
];

interface Filters { range: string; level: string; module: string; user: string; keyword: string; }
const EMPTY: Filters = { range: "24h", level: "all", module: "all", user: "all", keyword: "" };

export function Logs() {
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [detail, setDetail] = useState<Detail | null>(null);
  const [clearOpen, setClearOpen] = useState(false);

  const systemRows = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return SYSTEM_EVENTS.filter((r) => {
      if (filters.level !== "all" && r.level !== filters.level) return false;
      if (filters.module !== "all" && r.module !== filters.module) return false;
      if (kw && !`${r.type} ${r.message} ${r.source} ${r.object}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [filters]);

  const auditRows = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return AUDIT_LOGS.filter((r) => {
      if (filters.user !== "all" && r.user !== filters.user) return false;
      if (filters.level !== "all" && r.result !== filters.level) return false;
      if (filters.module !== "all" && !r.resource.toLowerCase().includes(filters.module)) return false;
      if (kw && !`${r.user} ${r.action} ${r.resource} ${r.detail}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [filters]);

  return (
    <>
      <PageHeader
        title="事件日志"
        en="Logs"
        description="查看系统事件、连接事件、配置变更与审计日志。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新日志")}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button variant="outline" size="sm" className="gap-1.5" onClick={() => toast.message("已导出日志（演示）")}>
              <Download className="size-3.5" />导出
            </Button>
            <Button variant="destructive" size="sm" className="gap-1.5" onClick={() => setClearOpen(true)}>
              <Trash2 className="size-3.5" />清理日志
            </Button>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Select value={filters.range} onValueChange={(v) => setFilters((f) => ({ ...f, range: v }))}>
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="时间范围" /></SelectTrigger>
            <SelectContent><SelectItem value="1h">最近 1 小时</SelectItem><SelectItem value="24h">最近 24 小时</SelectItem><SelectItem value="7d">最近 7 天</SelectItem><SelectItem value="30d">最近 30 天</SelectItem></SelectContent>
          </Select>
          <Select value={filters.level} onValueChange={(v) => setFilters((f) => ({ ...f, level: v }))}>
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="级别 / 结果" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部级别</SelectItem><SelectItem value="info">info</SelectItem><SelectItem value="warning">warning</SelectItem><SelectItem value="error">error</SelectItem><SelectItem value="success">success</SelectItem><SelectItem value="failed">failed</SelectItem><SelectItem value="denied">denied</SelectItem></SelectContent>
          </Select>
          <Select value={filters.module} onValueChange={(v) => setFilters((f) => ({ ...f, module: v }))}>
            <SelectTrigger className="h-8 w-40"><SelectValue placeholder="模块" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部模块</SelectItem><SelectItem value="system">system</SelectItem><SelectItem value="transport">transport</SelectItem><SelectItem value="polling">polling</SelectItem><SelectItem value="conversion">conversion</SelectItem><SelectItem value="auth">auth</SelectItem><SelectItem value="config">config</SelectItem></SelectContent>
          </Select>
          <Select value={filters.user} onValueChange={(v) => setFilters((f) => ({ ...f, user: v }))}>
            <SelectTrigger className="h-8 w-40"><SelectValue placeholder="用户" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部用户</SelectItem><SelectItem value="张工 / Admin">张工 / Admin</SelectItem><SelectItem value="李工 / Operator">李工 / Operator</SelectItem><SelectItem value="王工 / Viewer">王工 / Viewer</SelectItem></SelectContent>
          </Select>
          <Input className="h-8 w-64" placeholder="搜索消息 / 动作 / 资源 / 请求 ID" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">当前用户：张工 / Admin</span>
        </div>

        <Tabs defaultValue="events" className="space-y-3">
          <TabsList>
            <TabsTrigger value="events">System Events</TabsTrigger>
            <TabsTrigger value="audit">Audit Logs</TabsTrigger>
          </TabsList>
          <TabsContent value="events" className="rounded-md border border-border bg-card">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>时间</TableHead><TableHead>级别</TableHead><TableHead>模块</TableHead>
                  <TableHead>事件类型</TableHead><TableHead>消息</TableHead><TableHead>来源</TableHead>
                  <TableHead className="text-right">操作</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {systemRows.map((r) => (
                  <TableRow key={r.id}>
                    <TableCell className="text-xs text-muted-foreground">{r.time}</TableCell>
                    <TableCell><Badge variant="outline" className={STATUS_META[r.level].cls}>{STATUS_META[r.level].label}</Badge></TableCell>
                    <TableCell className="font-mono text-xs">{r.module}</TableCell>
                    <TableCell className="font-mono text-xs">{r.type}</TableCell>
                    <TableCell>{r.message}</TableCell>
                    <TableCell className="text-xs text-muted-foreground">{r.source}</TableCell>
                    <TableCell className="text-right"><Button variant="ghost" size="icon" className="size-7" title="详情" onClick={() => setDetail({ kind: "event", row: r })}><Eye className="size-3.5" /></Button></TableCell>
                  </TableRow>
                ))}
                {systemRows.length === 0 && (
                  <TableRow><TableCell colSpan={7} className="h-24 text-center text-muted-foreground">没有匹配的系统事件</TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TabsContent>
          <TabsContent value="audit" className="rounded-md border border-border bg-card">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>时间</TableHead><TableHead>用户</TableHead><TableHead>动作</TableHead>
                  <TableHead>资源</TableHead><TableHead>结果</TableHead><TableHead>IP</TableHead><TableHead>详情</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {auditRows.map((r) => (
                  <TableRow key={r.id} className="cursor-pointer" onClick={() => setDetail({ kind: "audit", row: r })}>
                    <TableCell className="text-xs text-muted-foreground">{r.time}</TableCell>
                    <TableCell>{r.user}</TableCell>
                    <TableCell className="font-mono text-xs">{r.action}</TableCell>
                    <TableCell>{r.resource}</TableCell>
                    <TableCell><Badge variant="outline" className={STATUS_META[r.result].cls}>{STATUS_META[r.result].label}</Badge></TableCell>
                    <TableCell className="font-mono text-xs">{r.ip}</TableCell>
                    <TableCell className="text-muted-foreground">{r.detail}</TableCell>
                  </TableRow>
                ))}
                {auditRows.length === 0 && (
                  <TableRow><TableCell colSpan={7} className="h-24 text-center text-muted-foreground">没有匹配的审计日志</TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </TabsContent>
        </Tabs>
      </div>

      <Sheet open={!!detail} onOpenChange={(o) => !o && setDetail(null)}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{detail?.kind === "event" ? "事件详情" : "审计详情"}</SheetTitle>
            <SheetDescription>查看原始 payload、关联对象、请求 ID 与 Trace ID。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-5 overflow-y-auto px-4">
            {detail?.kind === "event" && (
              <>
                <Info label="事件详情">{detail.row.message}</Info>
                <Info label="关联对象">{detail.row.object}</Info>
                <Info label="请求 ID">{detail.row.requestId}</Info>
                <Info label="Trace ID">{detail.row.traceId}</Info>
                <Info label="原始 payload"><pre className="whitespace-pre-wrap rounded-md bg-muted p-3 text-xs">{detail.row.payload}</pre></Info>
              </>
            )}
            {detail?.kind === "audit" && (
              <>
                <Info label="事件详情">{detail.row.detail}</Info>
                <Info label="关联对象">{detail.row.resource}</Info>
                <Info label="请求 ID">{detail.row.requestId}</Info>
                <Info label="Trace ID">{detail.row.traceId}</Info>
                <Info label="原始 payload"><pre className="whitespace-pre-wrap rounded-md bg-muted p-3 text-xs">{detail.row.payload}</pre></Info>
              </>
            )}
          </div>
        </SheetContent>
      </Sheet>

      <Dialog open={clearOpen} onOpenChange={setClearOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认清理日志？</DialogTitle>
            <DialogDescription>将按当前保留策略清理过期系统事件和审计日志，该操作会写入审计日志。</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setClearOpen(false)}>取消</Button>
            <Button variant="destructive" onClick={() => { setClearOpen(false); toast.success("已提交日志清理任务"); }}>确认清理</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}

function Info({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="space-y-1">
      <Label className="text-sm text-muted-foreground">{label}</Label>
      <div className="text-sm">{children}</div>
    </div>
  );
}
