import { useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { Pause, Pencil, Play, Plus, RefreshCw, RotateCw, Trash2 } from "lucide-react";
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
import { Checkbox } from "../components/ui/checkbox";

type PollStatus = "running" | "paused" | "error";

interface PollTask {
  id: string;
  name: string;
  transport: string;
  pointCount: number;
  period: string;
  window: string;
  status: PollStatus;
  lastRun: string;
  nextRun: string;
  lastError: string;
  enabled: boolean;
}

const STATUS_META: Record<PollStatus, { label: string; cls: string }> = {
  running: { label: "运行中", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  paused: { label: "暂停", cls: "bg-muted text-muted-foreground border-border" },
  error: { label: "异常", cls: "bg-red-50 text-red-700 border-red-200" },
};

const SEED: PollTask[] = [
  { id: "poll-1", name: "锅炉温压快采", transport: "PLC-1 产线A", pointCount: 28, period: "300ms", window: "00:00-24:00", status: "running", lastRun: "2026-06-18 10:22:03", nextRun: "2026-06-18 10:22:04", lastError: "-", enabled: true },
  { id: "poll-2", name: "泵站压力巡检", transport: "PLC-1 产线A", pointCount: 16, period: "1s", window: "00:00-24:00", status: "running", lastRun: "2026-06-18 10:22:02", nextRun: "2026-06-18 10:22:03", lastError: "-", enabled: true },
  { id: "poll-3", name: "OPC-UA 电机状态", transport: "OPC-UA 产线B", pointCount: 42, period: "500ms", window: "06:00-23:00", status: "error", lastRun: "2026-06-18 10:21:58", nextRun: "2026-06-18 10:22:08", lastError: "timeout after 3000ms", enabled: true },
  { id: "poll-4", name: "S7 安全信号", transport: "S7-1500 产线C", pointCount: 12, period: "200ms", window: "00:00-24:00", status: "running", lastRun: "2026-06-18 10:22:03", nextRun: "2026-06-18 10:22:04", lastError: "-", enabled: true },
  { id: "poll-5", name: "循环计数低频采集", transport: "S7-1500 产线C", pointCount: 8, period: "5s", window: "08:00-20:00", status: "paused", lastRun: "2026-06-18 09:58:11", nextRun: "-", lastError: "-", enabled: false },
  { id: "poll-6", name: "模拟量校验任务", transport: "Modbus-TCP 试验台", pointCount: 10, period: "2s", window: "09:00-18:00", status: "paused", lastRun: "2026-06-18 09:45:20", nextRun: "-", lastError: "-", enabled: false },
];

const TRANSPORTS = ["PLC-1 产线A", "OPC-UA 产线B", "S7-1500 产线C", "Modbus-TCP 试验台"];
const POINTS = ["锅炉水温", "主泵压力", "运行状态", "电机转速", "急停信号", "循环计数"];

interface Filters { keyword: string; transport: string; status: string; period: string; }
const EMPTY: Filters = { keyword: "", transport: "all", status: "all", period: "all" };

export function Polling({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<PollTask[]>(SEED);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: PollTask | null }>({ open: false, mode: "create", row: null });
  const [del, setDel] = useState<PollTask | null>(null);

  const filtered = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return rows.filter((r) => {
      if (filters.transport !== "all" && r.transport !== filters.transport) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (filters.period !== "all" && r.period !== filters.period) return false;
      if (kw && !`${r.name} ${r.transport} ${r.lastError}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const running = rows.filter((r) => r.status === "running").length;
  const paused = rows.filter((r) => r.status === "paused").length;
  const errors = rows.filter((r) => r.status === "error").length;
  const nextRun = rows.find((r) => r.status === "running")?.nextRun ?? "-";

  const draft = () => onDraftIncrement?.();

  function toggleTask(row: PollTask) {
    setRows((prev) => prev.map((r) => (
      r.id === row.id
        ? { ...r, enabled: !r.enabled, status: r.enabled ? "paused" : "running", nextRun: r.enabled ? "-" : "2026-06-18 10:22:10" }
        : r
    )));
    draft();
    toast.success(row.enabled ? `已暂停「${row.name}」` : `已恢复「${row.name}」`);
  }

  return (
    <>
      <PageHeader
        title="轮询任务"
        en="Polling"
        description="管理采集点轮询周期、窗口与运行状态。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新轮询任务")}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!canWrite} onClick={() => toast.warning("已暂停全部轮询任务（演示）")}>
              <Pause className="size-3.5" />暂停全部
            </Button>
            <PermissionButton allowed={canWrite} onAction={() => setDrawer({ open: true, mode: "create", row: null })} size="sm">
              <Plus className="size-3.5" />新增轮询任务
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="grid gap-3 md:grid-cols-4">
          <SummaryCard title="运行中任务" value={`${running}`} note="采集线程正常执行" />
          <SummaryCard title="暂停任务" value={`${paused}`} note="等待人工恢复" />
          <SummaryCard title="异常任务" value={`${errors}`} note="需检查连接或超时配置" danger={errors > 0} />
          <SummaryCard title="下次执行时间" value={nextRun.split(" ").pop() ?? "-"} note={nextRun} />
        </div>

        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索任务 / Transport / 错误" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Select value={filters.transport} onValueChange={(v) => setFilters((f) => ({ ...f, transport: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部 Transport</SelectItem>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.status} onValueChange={(v) => setFilters((f) => ({ ...f, status: v }))}>
            <SelectTrigger className="h-8 w-32"><SelectValue placeholder="状态" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部状态</SelectItem><SelectItem value="running">运行中</SelectItem><SelectItem value="paused">暂停</SelectItem><SelectItem value="error">异常</SelectItem></SelectContent>
          </Select>
          <Select value={filters.period} onValueChange={(v) => setFilters((f) => ({ ...f, period: v }))}>
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="周期范围" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部周期</SelectItem><SelectItem value="200ms">200ms</SelectItem><SelectItem value="300ms">300ms</SelectItem><SelectItem value="500ms">500ms</SelectItem><SelectItem value="1s">1s</SelectItem><SelectItem value="2s">2s</SelectItem><SelectItem value="5s">5s</SelectItem></SelectContent>
          </Select>
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 个轮询任务</span>
        </div>

        <div className="rounded-md border border-border bg-card">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>任务名称</TableHead><TableHead>关联 Transport</TableHead><TableHead>点位数</TableHead>
                <TableHead>轮询周期</TableHead><TableHead>时间窗口</TableHead><TableHead>状态</TableHead>
                <TableHead>上次执行</TableHead><TableHead>下次执行</TableHead><TableHead>最近错误</TableHead>
                <TableHead className="text-right">操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {filtered.map((r) => (
                <TableRow key={r.id}>
                  <TableCell className="font-medium">{r.name}</TableCell>
                  <TableCell className="text-muted-foreground">{r.transport}</TableCell>
                  <TableCell>{r.pointCount}</TableCell>
                  <TableCell className="font-mono text-xs">{r.period}</TableCell>
                  <TableCell className="text-xs">{r.window}</TableCell>
                  <TableCell><Badge variant="outline" className={STATUS_META[r.status].cls}>{STATUS_META[r.status].label}</Badge></TableCell>
                  <TableCell className="text-xs text-muted-foreground">{r.lastRun}</TableCell>
                  <TableCell className="text-xs text-muted-foreground">{r.nextRun}</TableCell>
                  <TableCell className={r.lastError === "-" ? "text-xs text-muted-foreground" : "text-xs text-red-600"}>{r.lastError}</TableCell>
                  <TableCell className="text-right">
                    <div className="flex justify-end gap-1">
                      <Button variant="ghost" size="icon" className="size-7" title="立即执行" onClick={() => toast.success(`已触发「${r.name}」立即执行`)}><RotateCw className="size-3.5" /></Button>
                      <Button variant="ghost" size="icon" className="size-7" title={r.enabled ? "暂停" : "恢复"} disabled={!canWrite} onClick={() => toggleTask(r)}>{r.enabled ? <Pause className="size-3.5" /> : <Play className="size-3.5" />}</Button>
                      <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => setDrawer({ open: true, mode: "edit", row: r })}><Pencil className="size-3.5" /></Button>
                      <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(r)}><Trash2 className="size-3.5" /></Button>
                    </div>
                  </TableCell>
                </TableRow>
              ))}
              {filtered.length === 0 && (
                <TableRow><TableCell colSpan={10} className="h-24 text-center text-muted-foreground">没有匹配的轮询任务</TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </div>
      </div>

      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增轮询任务" : `编辑轮询任务 · ${drawer.row?.name}`}</SheetTitle>
            <SheetDescription>保存后进入未生效配置，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="任务名称"><Input defaultValue={drawer.row?.name} placeholder="如 锅炉温压快采" /></Field>
              <Field label="Transport">
                <Select defaultValue={drawer.row?.transport ?? TRANSPORTS[0]}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label="是否启用"><Switch defaultChecked={drawer.row?.enabled ?? true} /></Field>
            </Section>
            <Section title="点位选择">
              <div className="grid gap-2 rounded-md border border-border p-3">
                {POINTS.map((p, index) => (
                  <label key={p} className="flex items-center gap-2 text-sm">
                    <Checkbox defaultChecked={index < 3} />
                    <span>{p}</span>
                  </label>
                ))}
              </div>
            </Section>
            <Section title="调度配置">
              <Field label="轮询周期"><Input defaultValue={drawer.row?.period ?? "300ms"} placeholder="如 300ms / 1s" /></Field>
              <Field label="时间窗口"><Input defaultValue={drawer.row?.window ?? "00:00-24:00"} placeholder="如 08:00-20:00" /></Field>
              <Field label="超时时间"><Input defaultValue="3000ms" /></Field>
              <Field label="重试次数"><Input defaultValue="2" /></Field>
            </Section>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={() => { setDrawer((d) => ({ ...d, open: false })); draft(); toast.success("已保存为草稿，需到 Config & Apply 发布后生效"); }}>保存草稿</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除轮询任务？</DialogTitle>
            <DialogDescription>将删除「{del?.name}」。关联点位不会删除，但发布后该任务将停止调度。</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setDel(null)}>取消</Button>
            <Button variant="destructive" onClick={() => { setRows((p) => p.filter((x) => x.id !== del?.id)); setDel(null); draft(); toast.success("已删除，存在未生效配置"); }}>确认删除</Button>
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
