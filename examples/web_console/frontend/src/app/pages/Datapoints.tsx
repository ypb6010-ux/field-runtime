import { useMemo, useState } from "react";
import { toast } from "sonner";
import { Plus, RefreshCw, Upload, Download, Pencil, Trash2, Activity, History } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
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
  Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription, SheetFooter,
} from "../components/ui/sheet";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";
import { Label } from "../components/ui/label";
import { Switch } from "../components/ui/switch";

// ---- mock 数据模型（对应后端 C 组接口）----
type DpStatus = "ok" | "warning" | "error" | "disabled";
type DpType = "number" | "boolean" | "string";

interface Datapoint {
  id: string;
  name: string;
  addr: string;
  transport: string;
  type: DpType;
  unit: string;
  codec: string;
  mode: string;
  status: DpStatus;
  updatedAt: string;
  tags: string[];
}

const STATUS_META: Record<DpStatus, { label: string; cls: string }> = {
  ok: { label: "正常", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  warning: { label: "告警", cls: "bg-amber-50 text-amber-700 border-amber-200" },
  error: { label: "错误", cls: "bg-red-50 text-red-700 border-red-200" },
  disabled: { label: "禁用", cls: "bg-muted text-muted-foreground border-border" },
};

const SEED: Datapoint[] = [
  { id: "dp-1", name: "锅炉水温", addr: "HR0", transport: "PLC-1 产线A", type: "number", unit: "℃", codec: "scale 0.1", mode: "轮询 300ms", status: "ok", updatedAt: "2026-06-18 10:21:03", tags: ["锅炉", "温度"] },
  { id: "dp-2", name: "主泵压力", addr: "HR1", transport: "PLC-1 产线A", type: "number", unit: "bar", codec: "scale 0.001", mode: "轮询 300ms", status: "warning", updatedAt: "2026-06-18 10:21:02", tags: ["泵", "压力"] },
  { id: "dp-3", name: "运行状态", addr: "HR4", transport: "PLC-1 产线A", type: "string", unit: "-", codec: "enum run_state", mode: "轮询 300ms", status: "ok", updatedAt: "2026-06-18 10:21:01", tags: ["状态"] },
  { id: "dp-4", name: "电机转速", addr: "Sim_1", transport: "OPC-UA 产线B", type: "number", unit: "rpm", codec: "raw", mode: "订阅", status: "ok", updatedAt: "2026-06-18 10:20:58", tags: ["电机"] },
  { id: "dp-5", name: "急停信号", addr: "DB1.DBX10.0", transport: "S7-1500 产线C", type: "boolean", unit: "-", codec: "bit", mode: "轮询 1s", status: "error", updatedAt: "2026-06-18 10:19:40", tags: ["安全"] },
  { id: "dp-6", name: "循环计数", addr: "DB1.DBD6", transport: "S7-1500 产线C", type: "number", unit: "次", codec: "u32 ABCD", mode: "轮询 1s", status: "disabled", updatedAt: "2026-06-18 09:58:11", tags: [] },
];

const TRANSPORTS = ["PLC-1 产线A", "OPC-UA 产线B", "S7-1500 产线C"];

interface Filters { keyword: string; transport: string; type: string; status: string; }
const EMPTY: Filters = { keyword: "", transport: "all", type: "all", status: "all" };

export function Datapoints({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<Datapoint[]>(SEED);
  const [filters, setFilters] = useState<Filters>(EMPTY);
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; row: Datapoint | null }>({ open: false, mode: "create", row: null });
  const [del, setDel] = useState<Datapoint | null>(null);

  const filtered = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return rows.filter((r) => {
      if (filters.transport !== "all" && r.transport !== filters.transport) return false;
      if (filters.type !== "all" && r.type !== filters.type) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (kw && !`${r.name} ${r.addr} ${r.tags.join(" ")}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const draft = () => { onDraftIncrement?.(); };

  return (
    <>
      <PageHeader
        title="采集点"
        en="Datapoints"
        description="管理设备点位、地址、数据类型与编解码配置。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新列表")}><RefreshCw className="size-3.5" />刷新</Button>
            <Button variant="outline" size="sm" className="gap-1.5" onClick={() => toast.message("批量导入（演示）")}><Upload className="size-3.5" />批量导入</Button>
            <Button variant="outline" size="sm" className="gap-1.5" onClick={() => toast.message("批量导出（演示）")}><Download className="size-3.5" />批量导出</Button>
            <PermissionButton allowed={canWrite} onAction={() => setDrawer({ open: true, mode: "create", row: null })} size="sm"><Plus className="size-3.5" />新增采集点</PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        {/* Filter Bar */}
        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索点位名称 / 地址 / 标签" value={filters.keyword} onChange={(e) => setFilters((f) => ({ ...f, keyword: e.target.value }))} />
          <Select value={filters.transport} onValueChange={(v) => setFilters((f) => ({ ...f, transport: v }))}>
            <SelectTrigger className="h-8 w-44"><SelectValue placeholder="Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部 Transport</SelectItem>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
          </Select>
          <Select value={filters.type} onValueChange={(v) => setFilters((f) => ({ ...f, type: v }))}>
            <SelectTrigger className="h-8 w-36"><SelectValue placeholder="数据类型" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部类型</SelectItem><SelectItem value="number">number</SelectItem><SelectItem value="boolean">boolean</SelectItem><SelectItem value="string">string</SelectItem></SelectContent>
          </Select>
          <Select value={filters.status} onValueChange={(v) => setFilters((f) => ({ ...f, status: v }))}>
            <SelectTrigger className="h-8 w-32"><SelectValue placeholder="状态" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部状态</SelectItem><SelectItem value="ok">正常</SelectItem><SelectItem value="warning">告警</SelectItem><SelectItem value="error">错误</SelectItem><SelectItem value="disabled">禁用</SelectItem></SelectContent>
          </Select>
          <Button variant="ghost" size="sm" onClick={() => setFilters(EMPTY)}>重置</Button>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 个点位</span>
        </div>

        {/* Table */}
        <div className="rounded-md border border-border bg-card">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>点位名称</TableHead><TableHead>地址</TableHead><TableHead>Transport</TableHead>
                <TableHead>类型</TableHead><TableHead>单位</TableHead><TableHead>编解码</TableHead>
                <TableHead>采集模式</TableHead><TableHead>状态</TableHead><TableHead>最近更新</TableHead>
                <TableHead className="text-right">操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {filtered.map((r) => (
                <TableRow key={r.id}>
                  <TableCell className="font-medium">{r.name}</TableCell>
                  <TableCell className="font-mono text-xs">{r.addr}</TableCell>
                  <TableCell className="text-muted-foreground">{r.transport}</TableCell>
                  <TableCell>{r.type}</TableCell>
                  <TableCell>{r.unit}</TableCell>
                  <TableCell className="text-xs">{r.codec}</TableCell>
                  <TableCell className="text-xs">{r.mode}</TableCell>
                  <TableCell><Badge variant="outline" className={STATUS_META[r.status].cls}>{STATUS_META[r.status].label}</Badge></TableCell>
                  <TableCell className="text-xs text-muted-foreground">{r.updatedAt}</TableCell>
                  <TableCell className="text-right">
                    <div className="flex justify-end gap-1">
                      <Button variant="ghost" size="icon" className="size-7" title="实时" onClick={() => toast.message(`查看「${r.name}」实时`)}><Activity className="size-3.5" /></Button>
                      <Button variant="ghost" size="icon" className="size-7" title="历史" onClick={() => toast.message(`查看「${r.name}」历史`)}><History className="size-3.5" /></Button>
                      <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => setDrawer({ open: true, mode: "edit", row: r })}><Pencil className="size-3.5" /></Button>
                      <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(r)}><Trash2 className="size-3.5" /></Button>
                    </div>
                  </TableCell>
                </TableRow>
              ))}
              {filtered.length === 0 && (
                <TableRow><TableCell colSpan={10} className="h-24 text-center text-muted-foreground">没有匹配的采集点</TableCell></TableRow>
              )}
            </TableBody>
          </Table>
        </div>
      </div>

      {/* 新增 / 编辑 Drawer */}
      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增采集点" : `编辑采集点 · ${drawer.row?.name}`}</SheetTitle>
            <SheetDescription>保存后进入未生效配置，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-6 overflow-y-auto px-4">
            <Section title="基本信息">
              <Field label="点位名称"><Input defaultValue={drawer.row?.name} placeholder="如 锅炉水温" /></Field>
              <Field label="标签"><Input defaultValue={drawer.row?.tags.join(", ")} placeholder="逗号分隔" /></Field>
              <Field label="启用"><Switch defaultChecked={drawer.row?.status !== "disabled"} /></Field>
            </Section>
            <Section title="地址配置">
              <Field label="所属 Transport">
                <Select defaultValue={drawer.row?.transport ?? TRANSPORTS[0]}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent>{TRANSPORTS.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label="地址"><Input defaultValue={drawer.row?.addr} placeholder="如 HR0 / Sim_1 / DB1.DBW0" /></Field>
              <Field label="采集周期"><Input defaultValue="300ms" /></Field>
            </Section>
            <Section title="数据配置">
              <Field label="数据类型">
                <Select defaultValue={drawer.row?.type ?? "number"}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="number">number</SelectItem><SelectItem value="boolean">boolean</SelectItem><SelectItem value="string">string</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="单位"><Input defaultValue={drawer.row?.unit} placeholder="如 ℃ / bar" /></Field>
              <Field label="精度"><Input defaultValue="2" /></Field>
            </Section>
            <Section title="编解码配置">
              <Field label="字节序 / scale / offset"><Input defaultValue={drawer.row?.codec} placeholder="如 scale 0.1" /></Field>
              <Field label="表达式 expression"><Input placeholder="可选" /></Field>
            </Section>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={() => { setDrawer((d) => ({ ...d, open: false })); draft(); toast.success("已保存为草稿，需到 Config & Apply 发布后生效"); }}>保存草稿</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      {/* 删除确认 */}
      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除采集点？</DialogTitle>
            <DialogDescription>将删除「{del?.name}」（{del?.addr}）。该操作会记入未生效配置，发布后生效。</DialogDescription>
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

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="space-y-3">
      <div className="text-sm font-medium text-foreground">{title}</div>
      <div className="space-y-3">{children}</div>
    </div>
  );
}
function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="grid grid-cols-[120px_1fr] items-center gap-3">
      <Label className="text-sm text-muted-foreground">{label}</Label>
      <div>{children}</div>
    </div>
  );
}
