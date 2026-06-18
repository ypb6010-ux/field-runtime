import { useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { FlaskConical, Pause, Pencil, Play, Plus, RefreshCw, Trash2 } from "lucide-react";
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

interface ConversionRule {
  id: string;
  name: string;
  sourceTransport: string;
  sourcePoint: string;
  targetTransport: string;
  targetPoint: string;
  transform: string;
  status: RuleStatus;
  lastRun: string;
  successRate: string;
  remark: string;
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

const SEED: ConversionRule[] = [
  { id: "rule-201", name: "锅炉水温上报 MQTT", sourceTransport: "PLC-1 产线A", sourcePoint: "锅炉水温", targetTransport: "MQTT 产线B 网关", targetPoint: "factory/a/boiler/temp", transform: "scale 0.1", status: "enabled", lastRun: "2026-06-18 10:22:04", successRate: "99.8%", remark: "温度单位转 ℃" },
  { id: "rule-202", name: "主泵压力写入 OPC", sourceTransport: "PLC-1 产线A", sourcePoint: "主泵压力", targetTransport: "OPC-UA 产线B", targetPoint: "Pump.Pressure", transform: "scale 0.001", status: "enabled", lastRun: "2026-06-18 10:22:03", successRate: "99.2%", remark: "bar 标准化" },
  { id: "rule-203", name: "运行状态枚举转换", sourceTransport: "PLC-1 产线A", sourcePoint: "运行状态", targetTransport: "MQTT 产线B 网关", targetPoint: "factory/a/state", transform: "enum mapping", status: "enabled", lastRun: "2026-06-18 10:22:02", successRate: "100%", remark: "0/1/2 到文本状态" },
  { id: "rule-204", name: "电机转速限幅", sourceTransport: "OPC-UA 产线B", sourcePoint: "电机转速", targetTransport: "S7-1500 产线C", targetPoint: "DB1.DBD20", transform: "expression clamp", status: "error", lastRun: "2026-06-18 10:21:55", successRate: "94.1%", remark: "目标写入超时" },
  { id: "rule-205", name: "急停信号透传", sourceTransport: "S7-1500 产线C", sourcePoint: "急停信号", targetTransport: "MQTT 产线B 网关", targetPoint: "factory/c/safety/estop", transform: "direct", status: "enabled", lastRun: "2026-06-18 10:22:01", successRate: "100%", remark: "安全事件上报" },
  { id: "rule-206", name: "循环计数归档", sourceTransport: "S7-1500 产线C", sourcePoint: "循环计数", targetTransport: "HTTP Archive", targetPoint: "/data/cycle-count", transform: "fallback 0", status: "disabled", lastRun: "2026-06-18 09:58:11", successRate: "-", remark: "维护窗口暂停" },
];

const ACTIVITIES: Activity[] = [
  { id: "act-1", time: "10:22:04.231", rule: "锅炉水温上报 MQTT", input: "713", output: "71.3 ℃", status: "success", error: "-" },
  { id: "act-2", time: "10:22:04.118", rule: "主泵压力写入 OPC", input: "16520", output: "16.52 bar", status: "success", error: "-" },
  { id: "act-3", time: "10:22:03.904", rule: "运行状态枚举转换", input: "1", output: "running", status: "success", error: "-" },
  { id: "act-4", time: "10:21:55.622", rule: "电机转速限幅", input: "1480", output: "-", status: "failed", error: "target write timeout" },
  { id: "act-5", time: "10:21:40.081", rule: "循环计数归档", input: "89210", output: "-", status: "skipped", error: "rule disabled" },
];

const TRANSPORTS = ["PLC-1 产线A", "OPC-UA 产线B", "S7-1500 产线C", "MQTT 产线B 网关", "HTTP Archive"];

interface Filters { keyword: string; source: string; target: string; status: string; }
const EMPTY: Filters = { keyword: "", source: "all", target: "all", status: "all" };

export function Conversion({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<ConversionRule[]>(SEED);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: ConversionRule | null }>({ open: false, mode: "create", row: null });
  const [del, setDel] = useState<ConversionRule | null>(null);

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
  const failures = ACTIVITIES.filter((a) => a.status === "failed").length;
  const draft = () => onDraftIncrement?.();

  function toggleRule(rule: ConversionRule) {
    setRows((prev) => prev.map((r) => (
      r.id === rule.id ? { ...r, status: r.status === "enabled" ? "disabled" : "enabled" } : r
    )));
    draft();
    toast.success(rule.status === "enabled" ? `已停用「${rule.name}」` : `已启用「${rule.name}」`);
  }

  return (
    <>
      <PageHeader
        title="协议转换"
        en="Conversion"
        description="配置源点位到目标协议的映射、变换和实时活动。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新转换规则")}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!canWrite} onClick={() => toast.message("批量启用 / 停用（演示）")}>
              <Pause className="size-3.5" />启用 / 停用
            </Button>
            <PermissionButton allowed={canWrite} onAction={() => setDrawer({ open: true, mode: "create", row: null })} size="sm">
              <Plus className="size-3.5" />新增转换规则
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="grid gap-3 md:grid-cols-4">
          <SummaryCard title="启用规则" value={`${enabled}`} note={`共 ${rows.length} 条规则`} />
          <SummaryCard title="今日转换次数" value="128,430" note="较昨日 +6.8%" />
          <SummaryCard title="失败次数" value={`${failures}`} note="近 5 分钟活动" danger={failures > 0} />
          <SummaryCard title="实时活动速率" value="246/s" note="conversion/* WebSocket" />
        </div>

        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索规则 / 点位 / Topic / 变换" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Select value={filters.source} onValueChange={(v) => setFilters((f) => ({ ...f, source: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="源 Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部源 Transport</SelectItem>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.target} onValueChange={(v) => setFilters((f) => ({ ...f, target: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="目标 Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部目标 Transport</SelectItem>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.status} onValueChange={(v) => setFilters((f) => ({ ...f, status: v }))}>
            <SelectTrigger className="h-8 w-32"><SelectValue placeholder="状态" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部状态</SelectItem><SelectItem value="enabled">启用</SelectItem><SelectItem value="disabled">停用</SelectItem><SelectItem value="error">异常</SelectItem></SelectContent>
          </Select>
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 条转换规则</span>
        </div>

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
                    <TableCell className="text-muted-foreground">{r.targetTransport}</TableCell>
                    <TableCell className="font-mono text-xs">{r.targetPoint}</TableCell>
                    <TableCell className="text-xs">{r.transform}</TableCell>
                    <TableCell><Badge variant="outline" className={STATUS_META[r.status].cls}>{STATUS_META[r.status].label}</Badge></TableCell>
                    <TableCell className="text-xs text-muted-foreground">{r.lastRun}</TableCell>
                    <TableCell>{r.successRate}</TableCell>
                    <TableCell className="text-right">
                      <div className="flex justify-end gap-1">
                        <Button variant="ghost" size="icon" className="size-7" title="测试转换" onClick={() => toast.success(`测试「${r.name}」完成`)}><FlaskConical className="size-3.5" /></Button>
                        <Button variant="ghost" size="icon" className="size-7" title={r.status === "enabled" ? "停用" : "启用"} disabled={!canWrite} onClick={() => toggleRule(r)}>{r.status === "enabled" ? <Pause className="size-3.5" /> : <Play className="size-3.5" />}</Button>
                        <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => setDrawer({ open: true, mode: "edit", row: r })}><Pencil className="size-3.5" /></Button>
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
              {ACTIVITIES.map((a) => (
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
            </CardContent>
          </Card>
        </div>
      </div>

      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-xl">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增转换规则" : `编辑转换规则 · ${drawer.row?.name}`}</SheetTitle>
            <SheetDescription>保存后进入未生效配置，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="规则名称"><Input defaultValue={drawer.row?.name} placeholder="如 锅炉水温上报 MQTT" /></Field>
              <Field label="备注"><Textarea defaultValue={drawer.row?.remark} placeholder="规则用途或现场说明" /></Field>
              <Field label="是否启用"><Switch defaultChecked={drawer.row?.status !== "disabled"} /></Field>
            </Section>
            <Section title="源配置">
              <Field label="源 Transport"><TransportSelect value={drawer.row?.sourceTransport} /></Field>
              <Field label="源点位"><Input defaultValue={drawer.row?.sourcePoint} placeholder="如 锅炉水温" /></Field>
              <Field label="源字段"><Input defaultValue="value" /></Field>
            </Section>
            <Section title="目标配置">
              <Field label="目标 Transport"><TransportSelect value={drawer.row?.targetTransport ?? "MQTT 产线B 网关"} /></Field>
              <Field label="目标点位 / Topic"><Input defaultValue={drawer.row?.targetPoint} placeholder="如 factory/a/boiler/temp" /></Field>
              <Field label="目标字段"><Input defaultValue="payload.value" /></Field>
            </Section>
            <Section title="变换配置">
              <Field label="变换方式">
                <Select defaultValue={drawer.row?.transform.includes("enum") ? "enum" : drawer.row?.transform.includes("expression") ? "expression" : drawer.row?.transform.includes("direct") ? "direct" : "scale"}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="direct">直接映射</SelectItem><SelectItem value="scale">scale / offset</SelectItem><SelectItem value="expression">expression</SelectItem><SelectItem value="enum">enum mapping</SelectItem><SelectItem value="fallback">fallback value</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="表达式"><Input defaultValue={drawer.row?.transform} placeholder="如 value * 0.1" /></Field>
              <Field label="fallback"><Input defaultValue="0" /></Field>
            </Section>
            <Section title="测试转换">
              <Field label="输入示例值"><Input defaultValue="713" /></Field>
              <Field label="输出预览"><Input defaultValue="71.3 ℃" readOnly /></Field>
              <Field label="测试结果"><Badge variant="outline" className={ACTIVITY_META.success.cls}>success</Badge></Field>
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
            <DialogTitle>确认删除转换规则？</DialogTitle>
            <DialogDescription>将删除「{del?.name}」。发布后该源点位不再写入目标协议。</DialogDescription>
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

function TransportSelect({ value }: { value?: string }) {
  return (
    <Select defaultValue={value ?? TRANSPORTS[0]}>
      <SelectTrigger><SelectValue /></SelectTrigger>
      <SelectContent>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
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
