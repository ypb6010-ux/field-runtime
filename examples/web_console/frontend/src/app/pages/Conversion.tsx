import { useEffect, useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { AlertCircle, FlaskConical, Loader2, Pause, Pencil, Play, Plus, RefreshCw, Trash2 } from "lucide-react";
import { apiDelete, apiGet, apiPost } from "../api";
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
import { Textarea } from "../components/ui/textarea";

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
}

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
  remark: string;
  hits: number;
  enabled: boolean;
}

interface Activity {
  id: string;
  time: string;
  rule: string;
  input: string;
  output: string;
  status: ActivityStatus;
  error: string;
}

interface Form {
  id: string;
  name: string;
  remark: string;
  enabled: boolean;
  sourcePoint: string;
  targetTransport: string;
  targetPoint: string;
  transform: string;
}

const STATUS_META: Record<RuleStatus, { label: string; cls: string }> = {
  enabled: { label: "启用", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  disabled: { label: "停用", cls: "bg-muted text-muted-foreground border-border" },
  error: { label: "异常", cls: "bg-red-50 text-red-700 border-red-200" },
};

const ACTIVITY_META: Record<ActivityStatus, { label: string; cls: string }> = {
  success: { label: "success", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  failed: { label: "failed", cls: "bg-red-50 text-red-700 border-red-200" },
  skipped: { label: "skipped", cls: "bg-amber-50 text-amber-700 border-amber-200" },
};

const EMPTY_FORM: Form = {
  id: "",
  name: "",
  remark: "",
  enabled: true,
  sourcePoint: "",
  targetTransport: "",
  targetPoint: "0",
  transform: "scale 1",
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
  return match ? Number(match[0]) : 1;
}

export function Conversion({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<ConversionRule[]>([]);
  const [transports, setTransports] = useState<TransportRow[]>([]);
  const [activities, setActivities] = useState<Activity[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: ConversionRule | null }>({ open: false, mode: "create", row: null });
  const [form, setForm] = useState<Form>(EMPTY_FORM);
  const [del, setDel] = useState<ConversionRule | null>(null);

  const transportName = (id: string) => transports.find((t) => t.id === id)?.name || id;

  function mapRule(r: ConversionRow, stat?: ConversionStats): ConversionRule {
    const src = parseObject(r.source_json);
    const dst = parseObject(r.dest_json);
    const tr = parseObject(r.transform_json);
    const scale = numberValue(tr.scale, 1);
    const hits = numberValue(stat?.hits, 0);
    const targetTransport = String(dst.transport ?? "");
    const enabled = r.enabled === "1";
    return {
      id: r.id,
      name: r.name || r.id,
      sourceTransport: "-",
      sourcePoint: String(src.dp ?? ""),
      targetTransport,
      targetPoint: String(dst.addr ?? ""),
      transform: `scale ${scale}`,
      scale,
      status: enabled ? "enabled" : "disabled",
      lastRun: "-",
      successRate: `${hits} 次`,
      remark: r.trigger ? `trigger: ${r.trigger}` : "",
      hits,
      enabled,
    };
  }

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [rules, tps] = await Promise.all([apiGet<ConversionRow[]>("/conversions"), apiGet<TransportRow[]>("/transports")]);
      const stats = await Promise.all(rules.map((r) => apiGet<ConversionStats>(`/conversions/${encodeURIComponent(r.id)}/stats`).catch(() => ({ id: r.id, hits: 0 }))));
      const mapped = rules.map((r, index) => mapRule(r, stats[index]));
      setTransports(tps);
      setRows(mapped);
      setActivities(mapped.map((r) => ({
        id: `stats-${r.id}`,
        time: "-",
        rule: r.name,
        input: "-",
        output: `${r.hits} 次`,
        status: r.enabled ? (r.hits > 0 ? "success" : "skipped") : "skipped",
        error: r.enabled ? "-" : "rule disabled",
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

  const enabled = rows.filter((r) => r.status === "enabled").length;
  const failures = activities.filter((a) => a.status === "failed").length;
  const sourceOptions = useMemo(() => Array.from(new Set(rows.map((r) => r.sourceTransport))).filter(Boolean), [rows]);
  const draft = () => onDraftIncrement?.();

  function openCreate() {
    setForm({ ...EMPTY_FORM, id: `rule-${Date.now()}`, targetTransport: transports[0]?.id ?? "" });
    setDrawer({ open: true, mode: "create", row: null });
  }

  function openEdit(row: ConversionRule) {
    setForm({
      id: row.id,
      name: row.name,
      remark: row.remark,
      enabled: row.enabled,
      sourcePoint: row.sourcePoint,
      targetTransport: row.targetTransport,
      targetPoint: row.targetPoint,
      transform: row.transform,
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
    };
  }

  async function save() {
    try {
      if (drawer.mode === "edit") await apiDelete(`/conversions/${encodeURIComponent(form.id)}`);
      await apiPost("/conversions", toBody());
      setDrawer((d) => ({ ...d, open: false }));
      draft();
      toast.success("已保存为草稿，需到 Config & Apply 发布后生效");
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "保存失败");
    }
  }

  async function toggleRule(rule: ConversionRule) {
    try {
      await apiPost(`/conversions/${encodeURIComponent(rule.id)}/${rule.status === "enabled" ? "disable" : "enable"}`);
      draft();
      toast.success(rule.status === "enabled" ? `已停用「${rule.name}」` : `已启用「${rule.name}」`);
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
        title="协议转换"
        en="Conversion"
        description="配置源点位到目标协议的映射、变换和实时活动。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!canWrite} onClick={() => toast.message("请在列表中逐条启用或停用转换规则")}>
              <Pause className="size-3.5" />启用 / 停用
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
          <SummaryCard title="今日转换次数" value={`${rows.reduce((sum, r) => sum + r.hits, 0)}`} note="来自 /conversions/{id}/stats" />
          <SummaryCard title="失败次数" value={`${failures}`} note="当前统计未报告失败" danger={failures > 0} />
          <SummaryCard title="实时活动速率" value="-" note="等待实时转换事件" />
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
                    <TableHead>目标 Transport</TableHead><TableHead>目标点位 / Topic</TableHead><TableHead>变换方式</TableHead>
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
                          <Button variant="ghost" size="icon" className="size-7" title="读取统计" onClick={() => toast.success(`「${r.name}」已转换 ${r.hits} 次`)}><FlaskConical className="size-3.5" /></Button>
                          <Button variant="ghost" size="icon" className="size-7" title={r.status === "enabled" ? "停用" : "启用"} disabled={!canWrite} onClick={() => toggleRule(r)}>{r.status === "enabled" ? <Pause className="size-3.5" /> : <Play className="size-3.5" />}</Button>
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
                <CardTitle className="text-sm">实时活动</CardTitle>
              </CardHeader>
              <CardContent className="space-y-3 px-4 pb-4">
                {activities.map((a) => (
                  <div key={a.id} className="rounded-md border border-border p-3">
                    <div className="flex items-center justify-between gap-2">
                      <div className="text-xs text-muted-foreground">{a.time}</div>
                      <Badge variant="outline" className={ACTIVITY_META[a.status].cls}>{ACTIVITY_META[a.status].label}</Badge>
                    </div>
                    <div className="mt-1 text-sm font-medium">{a.rule}</div>
                    <div className="mt-2 grid grid-cols-2 gap-2 text-xs">
                      <div><span className="text-muted-foreground">输入：</span>{a.input}</div>
                      <div><span className="text-muted-foreground">输出：</span>{a.output}</div>
                    </div>
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
            <SheetDescription>保存后进入未生效配置，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="规则 ID"><Input value={form.id} disabled={drawer.mode === "edit"} onChange={(e) => setForm((f) => ({ ...f, id: e.target.value }))} placeholder="如 rule-temperature" /></Field>
              <Field label="规则名称"><Input value={form.name} onChange={(e) => setForm((f) => ({ ...f, name: e.target.value }))} placeholder="如 锅炉水温上报 MQTT" /></Field>
              <Field label="备注"><Textarea value={form.remark} onChange={(e) => setForm((f) => ({ ...f, remark: e.target.value }))} placeholder="规则用途或现场说明" /></Field>
              <Field label="是否启用"><Switch checked={form.enabled} onCheckedChange={(v) => setForm((f) => ({ ...f, enabled: v }))} /></Field>
            </Section>
            <Section title="源配置">
              <Field label="源 Transport"><TransportSelect value="-" transports={[{ id: "-", name: "-" }]} /></Field>
              <Field label="源点位"><Input value={form.sourcePoint} onChange={(e) => setForm((f) => ({ ...f, sourcePoint: e.target.value }))} placeholder="如 datapoint.temperature" /></Field>
              <Field label="源字段"><Input value="value" readOnly /></Field>
            </Section>
            <Section title="目标配置">
              <Field label="目标 Transport"><TransportSelect value={form.targetTransport} transports={transports} onChange={(v) => setForm((f) => ({ ...f, targetTransport: v }))} /></Field>
              <Field label="目标点位 / Topic"><Input type="number" value={form.targetPoint} onChange={(e) => setForm((f) => ({ ...f, targetPoint: e.target.value }))} placeholder="写入地址 addr" /></Field>
              <Field label="目标字段"><Input value="addr" readOnly /></Field>
            </Section>
            <Section title="变换配置">
              <Field label="变换方式">
                <Select value="scale">
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="scale">scale / offset</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="表达式"><Input value={form.transform} onChange={(e) => setForm((f) => ({ ...f, transform: e.target.value }))} placeholder="如 scale 0.1" /></Field>
              <Field label="fallback"><Input value="0" readOnly /></Field>
            </Section>
            <Section title="测试转换">
              <Field label="输入示例值"><Input value="-" readOnly /></Field>
              <Field label="输出预览"><Input value="保存后由后端转换引擎执行" readOnly /></Field>
              <Field label="测试结果"><Badge variant="outline" className={ACTIVITY_META.skipped.cls}>skipped</Badge></Field>
            </Section>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={save} disabled={!form.id || !form.name || !form.sourcePoint || !form.targetTransport}>保存草稿</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除转换规则？</DialogTitle>
            <DialogDescription>将删除「{del?.name}」。发布后该源点位不再写入目标协议。</DialogDescription>
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
