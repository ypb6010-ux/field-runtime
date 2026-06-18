import { useEffect, useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { AlertCircle, Download, Eye, Loader2, RefreshCw, Trash2 } from "lucide-react";
import { apiGet } from "../api";
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

type LogLevel = "debug" | "info" | "warning" | "error";

interface SystemEvent {
  ts: string;
  level: string;
  source: string;
  code: string;
  message: string;
}

interface AuditLog {
  ts: string;
  user: string;
  action: string;
  target: string;
  detail_json: string;
}

type Detail =
  | { kind: "event"; row: SystemEvent }
  | { kind: "audit"; row: AuditLog };

const STATUS_META: Record<LogLevel, { label: string; cls: string }> = {
  debug: { label: "debug", cls: "bg-muted text-muted-foreground border-border" },
  info: { label: "info", cls: "bg-sky-50 text-sky-700 border-sky-200" },
  warning: { label: "warning", cls: "bg-amber-50 text-amber-700 border-amber-200" },
  error: { label: "error", cls: "bg-red-50 text-red-700 border-red-200" },
};

interface Filters { range: string; level: string; source: string; user: string; keyword: string; }
const EMPTY: Filters = { range: "24h", level: "all", source: "all", user: "all", keyword: "" };

const formatTs = (ts: string) => (ts ? new Date(+ts).toLocaleString() : "-");
const rowKey = (parts: string[]) => parts.join("|");
const levelMeta = (level: string) => STATUS_META[level as LogLevel] ?? { label: level || "-", cls: "bg-muted text-muted-foreground border-border" };

export function Logs() {
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [events, setEvents] = useState<SystemEvent[]>([]);
  const [audit, setAudit] = useState<AuditLog[]>([]);
  const [eventsLoading, setEventsLoading] = useState(true);
  const [auditLoading, setAuditLoading] = useState(true);
  const [eventsError, setEventsError] = useState<string | null>(null);
  const [auditError, setAuditError] = useState<string | null>(null);
  const [detail, setDetail] = useState<Detail | null>(null);
  const [clearOpen, setClearOpen] = useState(false);

  async function loadEvents(level = filters.level) {
    setEventsLoading(true);
    setEventsError(null);
    try {
      const p = new URLSearchParams({ level: level === "all" ? "" : level, page: "1", size: "100" });
      setEvents(await apiGet<SystemEvent[]>(`/system/events?${p.toString()}`));
    } catch (e) {
      setEventsError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setEventsLoading(false);
    }
  }

  async function loadAudit() {
    setAuditLoading(true);
    setAuditError(null);
    try {
      setAudit(await apiGet<AuditLog[]>("/audit?page=1&size=100"));
    } catch (e) {
      setAuditError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setAuditLoading(false);
    }
  }

  async function loadAll() {
    await Promise.all([loadEvents(), loadAudit()]);
  }

  useEffect(() => { loadEvents(); }, [filters.level]);
  useEffect(() => { loadAudit(); }, []);

  const sources = useMemo(() => Array.from(new Set(events.map((r) => r.source).filter(Boolean))), [events]);
  const users = useMemo(() => Array.from(new Set(audit.map((r) => r.user).filter(Boolean))), [audit]);

  const systemRows = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return events.filter((r) => {
      if (filters.source !== "all" && r.source !== filters.source) return false;
      if (kw && !`${r.code} ${r.message} ${r.source}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [events, filters]);

  const auditRows = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return audit.filter((r) => {
      if (filters.user !== "all" && r.user !== filters.user) return false;
      if (filters.source !== "all" && !r.target.toLowerCase().includes(filters.source.toLowerCase())) return false;
      if (kw && !`${r.user} ${r.action} ${r.target} ${r.detail_json}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [audit, filters]);

  return (
    <>
      <PageHeader
        title="事件日志"
        en="Logs"
        description="查看系统事件、连接事件、配置变更与审计日志。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={loadAll}>
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
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="事件级别" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部级别</SelectItem><SelectItem value="debug">debug</SelectItem><SelectItem value="info">info</SelectItem><SelectItem value="warning">warning</SelectItem><SelectItem value="error">error</SelectItem></SelectContent>
          </Select>
          <Select value={filters.source} onValueChange={(v) => setFilters((f) => ({ ...f, source: v }))}>
            <SelectTrigger className="h-8 w-40"><SelectValue placeholder="来源 / 目标" /></SelectTrigger>
            <SelectContent>
              <SelectItem value="all">全部来源</SelectItem>
              {sources.map((s) => <SelectItem key={s} value={s}>{s}</SelectItem>)}
            </SelectContent>
          </Select>
          <Select value={filters.user} onValueChange={(v) => setFilters((f) => ({ ...f, user: v }))}>
            <SelectTrigger className="h-8 w-40"><SelectValue placeholder="用户" /></SelectTrigger>
            <SelectContent>
              <SelectItem value="all">全部用户</SelectItem>
              {users.map((u) => <SelectItem key={u} value={u}>{u}</SelectItem>)}
            </SelectContent>
          </Select>
          <Input className="h-8 w-64" placeholder="搜索消息 / 动作 / 目标 / 来源" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">事件 {systemRows.length} 条 / 审计 {auditRows.length} 条</span>
        </div>

        <Tabs defaultValue="events" className="space-y-3">
          <TabsList>
            <TabsTrigger value="events">System Events</TabsTrigger>
            <TabsTrigger value="audit">Audit Logs</TabsTrigger>
          </TabsList>
          <TabsContent value="events" className="rounded-md border border-border bg-card">
            {eventsLoading ? (
              <LoadingState />
            ) : eventsError ? (
              <ErrorState message={eventsError} onRetry={() => loadEvents()} />
            ) : (
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>时间</TableHead><TableHead>级别</TableHead><TableHead>来源</TableHead>
                    <TableHead>事件代码</TableHead><TableHead>消息</TableHead>
                    <TableHead className="text-right">操作</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {systemRows.map((r) => {
                    const meta = levelMeta(r.level);
                    return (
                      <TableRow key={rowKey([r.ts, r.source, r.code, r.message])}>
                        <TableCell className="text-xs text-muted-foreground">{formatTs(r.ts)}</TableCell>
                        <TableCell><Badge variant="outline" className={meta.cls}>{meta.label}</Badge></TableCell>
                        <TableCell className="font-mono text-xs">{r.source}</TableCell>
                        <TableCell className="font-mono text-xs">{r.code}</TableCell>
                        <TableCell>{r.message}</TableCell>
                        <TableCell className="text-right"><Button variant="ghost" size="icon" className="size-7" title="详情" onClick={() => setDetail({ kind: "event", row: r })}><Eye className="size-3.5" /></Button></TableCell>
                      </TableRow>
                    );
                  })}
                  {systemRows.length === 0 && (
                    <TableRow><TableCell colSpan={6} className="h-24 text-center text-muted-foreground">没有匹配的系统事件</TableCell></TableRow>
                  )}
                </TableBody>
              </Table>
            )}
          </TabsContent>
          <TabsContent value="audit" className="rounded-md border border-border bg-card">
            {auditLoading ? (
              <LoadingState />
            ) : auditError ? (
              <ErrorState message={auditError} onRetry={loadAudit} />
            ) : (
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>时间</TableHead><TableHead>用户</TableHead><TableHead>动作</TableHead>
                    <TableHead>目标</TableHead><TableHead>详情</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {auditRows.map((r) => (
                    <TableRow key={rowKey([r.ts, r.user, r.action, r.target])} className="cursor-pointer" onClick={() => setDetail({ kind: "audit", row: r })}>
                      <TableCell className="text-xs text-muted-foreground">{formatTs(r.ts)}</TableCell>
                      <TableCell>{r.user}</TableCell>
                      <TableCell className="font-mono text-xs">{r.action}</TableCell>
                      <TableCell>{r.target}</TableCell>
                      <TableCell className="text-muted-foreground">{r.detail_json || "-"}</TableCell>
                    </TableRow>
                  ))}
                  {auditRows.length === 0 && (
                    <TableRow><TableCell colSpan={5} className="h-24 text-center text-muted-foreground">没有匹配的审计日志</TableCell></TableRow>
                  )}
                </TableBody>
              </Table>
            )}
          </TabsContent>
        </Tabs>
      </div>

      <Sheet open={!!detail} onOpenChange={(o) => !o && setDetail(null)}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{detail?.kind === "event" ? "事件详情" : "审计详情"}</SheetTitle>
            <SheetDescription>查看后端返回的原始字段。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-5 overflow-y-auto px-4">
            {detail?.kind === "event" && (
              <>
                <Info label="时间">{formatTs(detail.row.ts)}</Info>
                <Info label="级别">{detail.row.level}</Info>
                <Info label="来源">{detail.row.source}</Info>
                <Info label="事件代码">{detail.row.code}</Info>
                <Info label="消息">{detail.row.message}</Info>
              </>
            )}
            {detail?.kind === "audit" && (
              <>
                <Info label="时间">{formatTs(detail.row.ts)}</Info>
                <Info label="用户">{detail.row.user}</Info>
                <Info label="动作">{detail.row.action}</Info>
                <Info label="目标">{detail.row.target}</Info>
                <Info label="详情"><pre className="whitespace-pre-wrap rounded-md bg-muted p-3 text-xs">{detail.row.detail_json || "-"}</pre></Info>
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

function LoadingState() {
  return <div className="flex items-center justify-center gap-2 py-20 text-muted-foreground"><Loader2 className="size-4 animate-spin" />加载中…</div>;
}

function ErrorState({ message, onRetry }: { message: string; onRetry: () => void }) {
  return (
    <div className="flex flex-col items-center gap-3 py-16 text-center">
      <AlertCircle className="size-7 text-red-500" />
      <div className="text-sm font-medium">加载失败</div>
      <div className="text-sm text-muted-foreground">{message}</div>
      <Button variant="outline" size="sm" onClick={onRetry}>重试</Button>
    </div>
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
