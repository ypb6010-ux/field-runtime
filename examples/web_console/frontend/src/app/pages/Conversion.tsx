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

type RuleStatus = "enabled" | "disabled" | "error";
type ActivityStatus = "success" | "failed" | "skipped";

interface ConversionRow {
  id: string;
  name: string;
  enabled: string;
  source_json: string;
  dest_json: string;
  transform_json: string;
  trigger?: string;
  period_ms?: string;
}

interface TransportRow {
  id: string;
  name?: string;
  kind: string;
}

interface ConversionStats {
  id: string;
  hits: string | number;
  failures?: string | number;
  lastRunMs?: string | number;
  lastError?: string;
  skipped?: string | number;
  inFlight?: boolean;
}
interface DatapointRow { id: string; transport_id: string }

interface ConversionRule {
  id: string;
  name: string;
  sourceTransport: string;
  sourcePoint: string;
  targetTransport: string;
  targetPoint: string;
  transform: string;
  scale: number;
  status: RuleStatus;
  lastRun: string;
  successRate: string;
  hits: number;
  failures: number;
  lastError: string;
  trigger: "onChange" | "periodic";
  periodMs: number;
  enabled: boolean;
  inFlight: boolean;
}

interface Activity {
  id: string;
  time: string;
  rule: string;
  summary: string;
  status: ActivityStatus;
  error: string;
}

interface Form {
  id: string;
  name: string;
  enabled: boolean;
  sourcePoint: string;
  targetTransport: string;
  targetPoint: string;
  transform: string;
  trigger: "onChange" | "periodic";
  periodMs: number;
}

const STATUS_META: Record<RuleStatus, { label: string; cls: string }> = {
  enabled: { label: "启用", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  disabled: { label: "停用", cls: "bg-muted text-muted-foreground border-border" },
  error: { label: "异常", cls: "bg-red-50 text-red-700 border-red-200" },
};

const ACTIVITY_META: Record<ActivityStatus, { label: string; cls: string }> = {
  success: { label: "success", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  failed: { label: "failed", cls: "bg-red-50 text-red-700 border-red-200" },
  skipped: { label: "尚未执行", cls: "bg-amber-50 text-amber-700 border-amber-200" },
};

const EMPTY_FORM: Form = {
  id: "",
  name: "",
  enabled: true,
  sourcePoint: "",
  targetTransport: "",
  targetPoint: "0",
  transform: "scale 1",
  trigger: "onChange",
  periodMs: 1000,
};

interface Filters { keyword: string; source: string; target: string; status: string; }
const EMPTY: Filters = { keyword: "", source: "all", target: "all", status: "all" };

function parseObject(s: string): Record<string, unknown> {
  try {
    const value = JSON.parse(s) as unknown;
    return value && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : {};
  } catch {
    return {};
  }
}

function numberValue(v: unknown, fallback = 0) {
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? n : fallback;
}

function scaleFromText(text: string) {
  const match = text.match(/-?\d+(?:\.\d+)?/);
  return match ? Number(match[0]) : Number.NaN;
}

export function Conversion({ canWrite = true }: { canWrite?: boolean }) {
  const [rows, setRows] = useState<ConversionRule[]>([]);
  const [transports, setTransports] = useState<TransportRow[]>([]);
  const [datapoints, setDatapoints] = useState<DatapointRow[]>([]);
  const [activities, setActivities] = useState<Activity[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: ConversionRule | null }>({ open: false, mode: "create", row: null });
  const [form, setForm] = useState<Form>(EMPTY_FORM);
  const [del, setDel] = useState<ConversionRule | null>(null);

  const transportName = (id: string) => transports.find((t) => t.id === id)?.name || id;

  function mapRule(
    r: ConversionRow,
    points: DatapointRow[],
    stat?: ConversionStats,
  ): ConversionRule {
    const src = parseObject(r.source_json);
    const dst = parseObject(r.dest_json);
    const tr = parseObject(r.transform_json);
    const scale = numberValue(tr.scale, 1);
    const hits = numberValue(stat?.hits, 0);
    const failures = numberValue(stat?.failures, 0);
    const targetTransport = String(dst.transport ?? "");
    const enabled = r.enabled === "1";
    return {
      id: r.id,
      name: r.name || r.id,
      sourceTransport: points.find((point) => point.id === String(src.dp ?? ""))?.transport_id ?? "-",
      sourcePoint: String(src.dp ?? ""),
      targetTransport,
      targetPoint: String(dst.addr ?? ""),
      transform: `scale ${scale}`,
      scale,
      status: enabled ? (stat?.lastError ? "error" : "enabled") : "disabled",
      lastRun: stat?.lastRunMs ? new Date(numberValue(stat.lastRunMs)).toLocaleString("zh-CN", { hour12: false }) : "-",
      successRate: hits + failures > 0 ? `${Math.round((hits / (hits + failures)) * 100)}%` : "-",
      hits,
      failures,
      lastError: stat?.lastError ?? "",
      trigger: r.trigger === "periodic" ? "periodic" : "onChange",
      periodMs: numberValue(r.period_ms, 1000),
      enabled,
      inFlight: !!stat?.inFlight,
    };
  }

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [rules, tps, dps, stats] = await Promise.all([
        apiGet<ConversionRow[]>("/conversions"),
        apiGet<TransportRow[]>("/transports"),
        apiGet<DatapointRow[]>("/datapoints"),
        apiGet<ConversionStats[]>("/conversions/stats"),
      ]);
      setDatapoints(dps);
      const statsById = new Map(stats.map((stat) => [stat.id, stat]));
      const mapped = rules.map((rule) =>
        mapRule(rule, dps, statsById.get(rule.id)),
      );
      setTransports(tps);
      setRows(mapped);
      setActivities(mapped.map((r) => ({
        id: `stats-${r.id}`,
        time: r.lastRun,
        rule: r.name,
        summary: `成功 ${r.hits} 次 · 失败 ${r.failures} 次`,
        status: r.lastError ? "failed" : r.hits > 0 ? "success" : "skipped",
        error: r.lastError || "-",
      })));
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
      if (filters.source !== "all" && r.sourceTransport !== filters.source) return false;
      if (filters.target !== "all" && r.targetTransport !== filters.target) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (kw && !`${r.name} ${r.sourcePoint} ${r.targetPoint} ${r.transform}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const enabled = rows.filter((r) => r.enabled).length;
  const failures = rows.reduce((sum, row) => sum + row.failures, 0);
  const sourceOptions = useMemo(
    () => Array.from(new Set(rows.map((r) => r.sourceTransport))).filter((value) => value !== "-"),
    [rows],
  );
  function openCreate() {
    setForm({
      ...EMPTY_FORM,
      id: "",
      sourcePoint: datapoints[0]?.id ?? "",
      targetTransport: transports[0]?.id ?? "",
    });
    setDrawer({ open: true, mode: "create", row: null });
  }

  function openEdit(row: ConversionRule) {
    setForm({
      id: row.id,
      name: row.name,
      enabled: row.enabled,
      sourcePoint: row.sourcePoint,
      targetTransport: row.targetTransport,
      targetPoint: row.targetPoint,
      transform: row.transform,
      trigger: row.trigger,
      periodMs: row.periodMs,
    });
    setDrawer({ open: true, mode: "edit", row });
  }

  function toBody() {
    return {
      id: form.id,
      name: form.name,
      enabled: form.enabled ? 1 : 0,
      source: { dp: form.sourcePoint },
      dest: { transport: form.targetTransport, addr: Number(form.targetPoint) || 0 },
      transform: { scale: scaleFromText(form.transform) },
      trigger: form.trigger,
      period_ms: form.trigger === "periodic" ? form.periodMs : 0,
    };
  }

  async function save() {
    const address = Number(form.targetPoint);
    const scale = scaleFromText(form.transform);
    if (!isValidResourceId(form.id)) {
      toast.error("规则 ID 须为 1–128 字符，且不可含空白或 /\\?#%");
      return;
    }
    if (!Number.isInteger(address) || address < 0 || address > 65535) {
      toast.error("目标地址必须是 0..65535 的整数");
      return;
    }
    if (!Number.isFinite(scale) || scale === 0) {
      toast.error("scale 必须是非零有效数字");
      return;
    }
    if (
      form.trigger === "periodic"
      && (!Number.isInteger(form.periodMs) || form.periodMs < 100 || form.periodMs > 86400000)
    ) {
      toast.error("周期必须是 100..86400000 毫秒");
      return;
    }
    try {
      if (drawer.mode === "edit") {
        await apiPut(`/conversions/${encodeURIComponent(form.id)}`, toBody());
      } else {
        await apiPost("/conversions", toBody());
      }
      setDrawer((d) => ({ ...d, open: false }));
      toast.success("转换规则已保存并立即生效");
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "保存失败");
    }
  }

  async function toggleRule(rule: ConversionRule) {
    try {
      await apiPost(`/conversions/${encodeURIComponent(rule.id)}/${rule.enabled ? "disable" : "enable"}`);
      toast.success(rule.enabled ? `已停用「${rule.name}」` : `已启用「${rule.name}」`);
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "操作失败");
    }
  }

  async function doDelete() {
    if (!del) return;
    try {
      await apiDelete(`/conversions/${encodeURIComponent(del.id)}`);
      setDel(null);
      toast.success("转换规则已删除");
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "删除失败");
    }
  }

  return (
    <>
      <PageHeader
        title="协议转换"
        en="Conversion"
        description="配置源点位到目标寄存器的变换；规则保存后由转换引擎立即执行。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <PermissionButton allowed={canWrite} onAction={openCreate} size="sm">
              <Plus className="size-3.5" />新增转换规则
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="grid gap-3 md:grid-cols-4">
          <SummaryCard title="启用规则" value={`${enabled}`} note={`共 ${rows.length} 条规则`} />
          <SummaryCard title="本次进程成功写入" value={`${rows.reduce((sum, r) => sum + r.hits, 0)}`} note="来自 /conversions/{id}/stats" />
          <SummaryCard title="本次进程失败" value={`${failures}`} note="失败后按触发策略重试" danger={failures > 0} />
          <SummaryCard
            title="正在写入"
            value={`${rows.filter((row) => row.inFlight).length}`}
            note="当前尚未完成的目标写操作"
          />
        </div>

        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索规则 / 点位 / 地址 / 变换" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Select value={filters.source} onValueChange={(v) => setFilters((f) => ({ ...f, source: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="源 Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部源 Transport</SelectItem>{sourceOptions.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.target} onValueChange={(v) => setFilters((f) => ({ ...f, target: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="目标 Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部目标 Transport</SelectItem>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{transportName(t.id)}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.status} onValueChange={(v) => setFilters((f) => ({ ...f, status: v }))}>
            <SelectTrigger className="h-8 w-32"><SelectValue placeholder="状态" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部状态</SelectItem><SelectItem value="enabled">启用</SelectItem><SelectItem value="disabled">停用</SelectItem><SelectItem value="error">异常</SelectItem></SelectContent>
          </Select>
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 条转换规则</span>
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
          <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_420px]">
            <div className="rounded-md border border-border bg-card">
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>规则名称</TableHead><TableHead>源 Transport</TableHead><TableHead>源点位</TableHead>
                    <TableHead>目标 Transport</TableHead><TableHead>目标寄存器地址</TableHead><TableHead>变换方式</TableHead>
                    <TableHead>状态</TableHead><TableHead>最近执行</TableHead><TableHead>成功率</TableHead>
                    <TableHead className="text-right">操作</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {filtered.map((r) => (
                    <TableRow key={r.id}>
                      <TableCell className="font-medium">{r.name}</TableCell>
                      <TableCell className="text-muted-foreground">{r.sourceTransport}</TableCell>
                      <TableCell>{r.sourcePoint}</TableCell>
                      <TableCell className="text-muted-foreground">{transportName(r.targetTransport)}</TableCell>
                      <TableCell className="font-mono text-xs">{r.targetPoint}</TableCell>
                      <TableCell className="text-xs">{r.transform}</TableCell>
                      <TableCell><Badge variant="outline" className={STATUS_META[r.status].cls}>{STATUS_META[r.status].label}</Badge></TableCell>
                      <TableCell className="text-xs text-muted-foreground">{r.lastRun}</TableCell>
                      <TableCell>{r.successRate}</TableCell>
                      <TableCell className="text-right">
                        <div className="flex justify-end gap-1">
                          <Button variant="ghost" size="icon" className="size-7" title={r.enabled ? "停用" : "启用"} disabled={!canWrite} onClick={() => toggleRule(r)}>{r.enabled ? <Pause className="size-3.5" /> : <Play className="size-3.5" />}</Button>
                          <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => openEdit(r)}><Pencil className="size-3.5" /></Button>
                          <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(r)}><Trash2 className="size-3.5" /></Button>
                        </div>
                      </TableCell>
                    </TableRow>
                  ))}
                  {filtered.length === 0 && (
                    <TableRow><TableCell colSpan={10} className="h-24 text-center text-muted-foreground">没有匹配的转换规则</TableCell></TableRow>
                  )}
                </TableBody>
              </Table>
            </div>

            <Card className="rounded-md">
              <CardHeader className="px-4 pt-4">
                <CardTitle className="text-sm">本次进程执行摘要</CardTitle>
              </CardHeader>
              <CardContent className="space-y-3 px-4 pb-4">
                {activities.map((a) => (
                  <div key={a.id} className="rounded-md border border-border p-3">
                    <div className="flex items-center justify-between gap-2">
                      <div className="text-xs text-muted-foreground">{a.time}</div>
                      <Badge variant="outline" className={ACTIVITY_META[a.status].cls}>{ACTIVITY_META[a.status].label}</Badge>
                    </div>
                    <div className="mt-1 text-sm font-medium">{a.rule}</div>
                    <div className="mt-2 text-xs text-muted-foreground">{a.summary}</div>
                    {a.error !== "-" && <div className="mt-1 text-xs text-red-600">{a.error}</div>}
                  </div>
                ))}
                {activities.length === 0 && <div className="rounded-md border border-dashed border-border p-6 text-center text-sm text-muted-foreground">暂无转换活动</div>}
              </CardContent>
            </Card>
          </div>
        )}
      </div>

      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-xl">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增转换规则" : `编辑转换规则 · ${drawer.row?.name}`}</SheetTitle>
            <SheetDescription>保存后由转换引擎立即采用；写入失败会保留统计与最近错误。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="规则 ID"><Input value={form.id} disabled={drawer.mode === "edit"} onChange={(e) => setForm((f) => ({ ...f, id: e.target.value }))} placeholder="如 rule-temperature" /></Field>
              <Field label="规则名称"><Input value={form.name} onChange={(e) => setForm((f) => ({ ...f, name: e.target.value }))} placeholder="如 锅炉水温写入备用 PLC" /></Field>
              <Field label="是否启用"><Switch checked={form.enabled} onCheckedChange={(v) => setForm((f) => ({ ...f, enabled: v }))} /></Field>
            </Section>
            <Section title="源配置">
              <Field label="源点位">
                <Select value={form.sourcePoint} onValueChange={(value) => setForm((current) => ({ ...current, sourcePoint: value }))}>
                  <SelectTrigger><SelectValue placeholder="选择采集点" /></SelectTrigger>
                  <SelectContent>{datapoints.map((point) => <SelectItem key={point.id} value={point.id}>{point.id}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label="源字段"><Input value="value" readOnly /></Field>
            </Section>
            <Section title="目标配置">
              <Field label="目标 Transport"><TransportSelect value={form.targetTransport} transports={transports} onChange={(v) => setForm((f) => ({ ...f, targetTransport: v }))} /></Field>
              <Field label="目标寄存器地址"><Input type="number" value={form.targetPoint} onChange={(e) => setForm((f) => ({ ...f, targetPoint: e.target.value }))} placeholder="Holding Register 地址" /></Field>
              <Field label="目标字段"><Input value="addr" readOnly /></Field>
            </Section>
            <Section title="变换配置">
              <Field label="触发方式">
                <Select value={form.trigger} onValueChange={(value) => setForm((current) => ({ ...current, trigger: value as Form["trigger"] }))}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="onChange">值变化时</SelectItem><SelectItem value="periodic">固定周期</SelectItem></SelectContent>
                </Select>
              </Field>
              {form.trigger === "periodic" && (
                <Field label="周期 ms"><Input type="number" min={100} max={86400000} value={form.periodMs} onChange={(event) => setForm((current) => ({ ...current, periodMs: Number(event.target.value) }))} /></Field>
              )}
              <Field label="变换方式">
                <Select value="scale">
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="scale">比例系数 scale</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="表达式"><Input value={form.transform} onChange={(e) => setForm((f) => ({ ...f, transform: e.target.value }))} placeholder="如 scale 0.1" /></Field>
            </Section>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={save} disabled={!form.id || !form.name || !form.sourcePoint || !form.targetTransport}>保存并生效</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除转换规则？</DialogTitle>
            <DialogDescription>将立即删除「{del?.name}」，该源点位随后不再写入目标寄存器。</DialogDescription>
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

function TransportSelect({ value, transports, onChange }: { value?: string; transports: Pick<TransportRow, "id" | "name">[]; onChange?: (v: string) => void }) {
  return (
    <Select value={value ?? transports[0]?.id ?? ""} onValueChange={(v) => onChange?.(v)}>
      <SelectTrigger><SelectValue /></SelectTrigger>
      <SelectContent>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{t.name || t.id}</SelectItem>)}</SelectContent>
    </Select>
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
