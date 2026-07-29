import { useEffect, useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { AlertCircle, Loader2, Pause, Pencil, Play, Plus, RefreshCw, Trash2 } from "lucide-react";
import { apiDelete, apiGet, apiPost, apiPut, isValidResourceId } from "../api";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Badge } from "../components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "../components/ui/card";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../components/ui/select";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";
import {
  Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription, SheetFooter,
} from "../components/ui/sheet";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";
import { Label } from "../components/ui/label";
import { Switch } from "../components/ui/switch";

type PollStatus = "enabled" | "disabled";

interface PollRangeRow {
  id: string;
  transport_id: string;
  reg_table: string;
  start: string;
  count: string;
  period_ms: string;
  enabled: string;
}

interface TransportRow {
  id: string;
  name?: string;
  kind: string;
}

interface PollTask {
  id: string;
  transportId: string;
  transport: string;
  pointCount: number;
  periodMs: number;
  period: string;
  window: string;
  regTable: string;
  start: number;
  status: PollStatus;
  enabled: boolean;
}

interface Form {
  id: string;
  transport_id: string;
  reg_table: string;
  start: number;
  count: number;
  period_ms: number;
  enabled: boolean;
}

const STATUS_META: Record<PollStatus, { label: string; cls: string }> = {
  enabled: { label: "草稿启用", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  disabled: { label: "草稿禁用", cls: "bg-muted text-muted-foreground border-border" },
};

const EMPTY_FORM: Form = { id: "", transport_id: "", reg_table: "HR", start: 0, count: 1, period_ms: 1000, enabled: true };
const EMPTY: Filters = { keyword: "", transport: "all", status: "all", period: "all" };

interface Filters { keyword: string; transport: string; status: string; period: string; }

function formatPeriod(ms: number) {
  return ms >= 1000 && ms % 1000 === 0 ? `${ms / 1000}s` : `${ms}ms`;
}

function toBody(form: Form) {
  return {
    id: form.id,
    transport_id: form.transport_id,
    reg_table: form.reg_table,
    start: form.start,
    count: form.count,
    period_ms: form.period_ms,
    enabled: form.enabled ? 1 : 0,
  };
}

export function Polling({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<PollTask[]>([]);
  const [transports, setTransports] = useState<TransportRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: PollTask | null }>({ open: false, mode: "create", row: null });
  const [form, setForm] = useState<Form>(EMPTY_FORM);
  const [del, setDel] = useState<PollTask | null>(null);

  const transportName = (id: string) => transports.find((t) => t.id === id)?.name || id;

  function mapTask(r: PollRangeRow, tps: TransportRow[]): PollTask {
    const start = +r.start || 0;
    const count = +r.count || 0;
    const periodMs = +r.period_ms || 0;
    const enabled = r.enabled === "1";
    const tp = tps.find((t) => t.id === r.transport_id);
    return {
      id: r.id,
      transportId: r.transport_id,
      transport: tp?.name || r.transport_id,
      pointCount: count,
      periodMs,
      period: formatPeriod(periodMs),
      window: `${r.reg_table} ${start} - ${start + Math.max(count - 1, 0)}`,
      regTable: r.reg_table,
      start,
      status: enabled ? "enabled" : "disabled",
      enabled,
    };
  }

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [polls, tps] = await Promise.all([apiGet<PollRangeRow[]>("/poll_ranges"), apiGet<TransportRow[]>("/transports")]);
      setTransports(tps);
      setRows(polls.map((r) => mapTask(r, tps)));
    } catch (e) {
      setError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => { load(); }, []);

  const filtered = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return rows.filter((r) => {
      if (filters.transport !== "all" && r.transportId !== filters.transport) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (filters.period !== "all" && String(r.periodMs) !== filters.period) return false;
      if (kw && !`${r.id} ${r.transport} ${r.regTable} ${r.start}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const periodOptions = useMemo(() => Array.from(new Set(rows.map((r) => r.periodMs))).sort((a, b) => a - b), [rows]);
  const enabledCount = rows.filter((r) => r.enabled).length;
  const disabledCount = rows.length - enabledCount;
  const registerCount = rows.reduce((sum, row) => sum + row.pointCount, 0);

  const draft = () => onDraftIncrement?.();

  function openCreate() {
    setForm({ ...EMPTY_FORM, id: `poll-${Date.now()}`, transport_id: transports[0]?.id ?? "" });
    setDrawer({ open: true, mode: "create", row: null });
  }

  function openEdit(r: PollTask) {
    setForm({
      id: r.id,
      transport_id: r.transportId,
      reg_table: r.regTable,
      start: r.start,
      count: r.pointCount,
      period_ms: r.periodMs,
      enabled: r.enabled,
    });
    setDrawer({ open: true, mode: "edit", row: r });
  }

  async function save() {
    if (!isValidResourceId(form.id)) {
      toast.error("任务 ID 须为 1–128 字符，且不可含空白或 /\\?#%");
      return;
    }
    if (
      !Number.isInteger(form.start)
      || !Number.isInteger(form.count)
      || form.start < 0
      || form.start > 65535
      || form.count < 1
      || form.count > 125
      || form.start + form.count > 65536
    ) {
      toast.error("轮询范围必须位于 0..65535，且 count 为 1..125");
      return;
    }
    if (!Number.isInteger(form.period_ms) || form.period_ms < 1 || form.period_ms > 86400000) {
      toast.error("轮询周期必须为 1..86400000 ms");
      return;
    }
    try {
      if (drawer.mode === "create") await apiPost("/poll_ranges", toBody(form));
      else await apiPut(`/poll_ranges/${encodeURIComponent(form.id)}`, toBody(form));
      setDrawer((d) => ({ ...d, open: false }));
      draft();
      toast.success("已保存为草稿，需到 Config & Apply 发布后生效");
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "保存失败");
    }
  }

  async function toggleTask(row: PollTask) {
    const next: Form = {
      id: row.id,
      transport_id: row.transportId,
      reg_table: row.regTable,
      start: row.start,
      count: row.pointCount,
      period_ms: row.periodMs,
      enabled: !row.enabled,
    };
    try {
      await apiPut(`/poll_ranges/${encodeURIComponent(row.id)}`, toBody(next));
      draft();
      toast.success(row.enabled ? `已在草稿中禁用「${row.id}」` : `已在草稿中启用「${row.id}」`);
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "操作失败");
    }
  }

  async function doDelete() {
    if (!del) return;
    try {
      await apiDelete(`/poll_ranges/${encodeURIComponent(del.id)}`);
      setDel(null);
      draft();
      toast.success("已删除，存在未生效配置");
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "删除失败");
    }
  }

  return (
    <>
      <PageHeader
        title="轮询任务"
        en="Polling"
        description="管理轮询范围与周期；列表展示数据库草稿，发布后才影响运行时。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <PermissionButton allowed={canWrite} onAction={openCreate} size="sm">
              <Plus className="size-3.5" />新增轮询任务
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="grid gap-3 md:grid-cols-4">
          <SummaryCard title="轮询范围" value={`${rows.length}`} note="数据库草稿总数" />
          <SummaryCard title="草稿启用" value={`${enabledCount}`} note="发布后进入调度器" />
          <SummaryCard title="草稿禁用" value={`${disabledCount}`} note="发布后不创建任务" />
          <SummaryCard title="寄存器总数" value={`${registerCount}`} note="所有范围的 count 合计" />
        </div>

        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索任务 / Transport / 地址" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Select value={filters.transport} onValueChange={(v) => setFilters((f) => ({ ...f, transport: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部 Transport</SelectItem>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{transportName(t.id)}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.status} onValueChange={(v) => setFilters((f) => ({ ...f, status: v }))}>
            <SelectTrigger className="h-8 w-32"><SelectValue placeholder="状态" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部状态</SelectItem><SelectItem value="enabled">草稿启用</SelectItem><SelectItem value="disabled">草稿禁用</SelectItem></SelectContent>
          </Select>
          <Select value={filters.period} onValueChange={(v) => setFilters((f) => ({ ...f, period: v }))}>
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="周期范围" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部周期</SelectItem>{periodOptions.map((p) => <SelectItem key={p} value={String(p)}>{formatPeriod(p)}</SelectItem>)}</SelectContent>
          </Select>
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 个轮询任务</span>
        </div>

        {loading ? (
          <div className="flex items-center justify-center gap-2 rounded-md border border-border bg-card py-20 text-muted-foreground"><Loader2 className="size-4 animate-spin" />加载中…</div>
        ) : error ? (
          <div className="flex flex-col items-center gap-3 rounded-md border border-dashed border-border bg-card py-16 text-center">
            <AlertCircle className="size-7 text-red-500" /><div className="text-sm font-medium">加载失败</div>
            <div className="text-sm text-muted-foreground">{error}</div>
            <Button variant="outline" size="sm" onClick={load}>重试</Button>
          </div>
        ) : (
          <div className="rounded-md border border-border bg-card">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>任务名称</TableHead><TableHead>关联 Transport</TableHead><TableHead>点位数</TableHead>
                  <TableHead>轮询周期</TableHead><TableHead>寄存器范围</TableHead><TableHead>草稿状态</TableHead>
                  <TableHead className="text-right">操作</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {filtered.map((r) => (
                  <TableRow key={r.id}>
                    <TableCell className="font-medium">{r.id}</TableCell>
                    <TableCell className="text-muted-foreground">{r.transport}</TableCell>
                    <TableCell>{r.pointCount}</TableCell>
                    <TableCell className="font-mono text-xs">{r.period}</TableCell>
                    <TableCell className="text-xs">{r.window}</TableCell>
                    <TableCell><Badge variant="outline" className={STATUS_META[r.status].cls}>{STATUS_META[r.status].label}</Badge></TableCell>
                    <TableCell className="text-right">
                      <div className="flex justify-end gap-1">
                        <Button variant="ghost" size="icon" className="size-7" title={r.enabled ? "在草稿中禁用" : "在草稿中启用"} disabled={!canWrite} onClick={() => toggleTask(r)}>{r.enabled ? <Pause className="size-3.5" /> : <Play className="size-3.5" />}</Button>
                        <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => openEdit(r)}><Pencil className="size-3.5" /></Button>
                        <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(r)}><Trash2 className="size-3.5" /></Button>
                      </div>
                    </TableCell>
                  </TableRow>
                ))}
                {filtered.length === 0 && (
                  <TableRow><TableCell colSpan={7} className="h-24 text-center text-muted-foreground">没有匹配的轮询任务</TableCell></TableRow>
                )}
              </TableBody>
            </Table>
          </div>
        )}
      </div>

      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增轮询任务" : `编辑轮询任务 · ${drawer.row?.id}`}</SheetTitle>
            <SheetDescription>保存后进入未生效配置，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="任务 ID"><Input value={form.id} disabled={drawer.mode === "edit"} onChange={(e) => setForm((f) => ({ ...f, id: e.target.value }))} placeholder="如 poll-main-hr" /></Field>
              <Field label="Transport">
                <Select value={form.transport_id} onValueChange={(v) => setForm((f) => ({ ...f, transport_id: v }))}>
                  <SelectTrigger><SelectValue placeholder="选择 Transport" /></SelectTrigger>
                  <SelectContent>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{transportName(t.id)}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label="是否启用"><Switch checked={form.enabled} onCheckedChange={(v) => setForm((f) => ({ ...f, enabled: v }))} /></Field>
            </Section>
            <Section title="点位选择">
              <Field label="寄存器表">
                <Select value={form.reg_table} onValueChange={(v) => setForm((f) => ({ ...f, reg_table: v }))}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="HR">HR</SelectItem><SelectItem value="IR">IR</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="起始地址"><Input type="number" value={form.start} onChange={(e) => setForm((f) => ({ ...f, start: +e.target.value }))} /></Field>
              <Field label="数量"><Input type="number" min={1} max={125} value={form.count} onChange={(e) => setForm((f) => ({ ...f, count: +e.target.value }))} /></Field>
            </Section>
            <Section title="调度配置">
              <Field label="轮询周期"><Input type="number" min={1} max={86400000} value={form.period_ms} onChange={(e) => setForm((f) => ({ ...f, period_ms: +e.target.value }))} /></Field>
              <Field label="时间窗口"><Input value="后端按轮询范围持续调度" readOnly /></Field>
              <Field label="超时时间"><Input value="由协议连接配置决定" readOnly /></Field>
              <Field label="重试次数"><Input value="由协议连接配置决定" readOnly /></Field>
            </Section>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={save} disabled={!form.id || !form.transport_id || form.count < 1 || form.period_ms < 1}>保存草稿</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除轮询任务？</DialogTitle>
            <DialogDescription>将删除「{del?.id}」。关联点位不会删除，但发布后该任务将停止调度。</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setDel(null)}>取消</Button>
            <Button variant="destructive" onClick={doDelete}>确认删除</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}

function SummaryCard({ title, value, note, danger }: { title: string; value: string; note: string; danger?: boolean }) {
  return (
    <Card className="rounded-md">
      <CardHeader className="px-4 pt-4">
        <CardTitle className="text-sm text-muted-foreground">{title}</CardTitle>
      </CardHeader>
      <CardContent className="px-4 pb-4">
        <div className={danger ? "text-2xl font-semibold text-red-600" : "text-2xl font-semibold"}>{value}</div>
        <div className="mt-1 text-xs text-muted-foreground">{note}</div>
      </CardContent>
    </Card>
  );
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="space-y-3">
      <div className="text-sm font-medium text-foreground">{title}</div>
      <div className="space-y-3">{children}</div>
    </div>
  );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[120px_1fr] items-center gap-3">
      <Label className="text-sm text-muted-foreground">{label}</Label>
      <div>{children}</div>
    </div>
  );
}
