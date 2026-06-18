import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { apiGet, apiPost } from "../api";
import { CheckCircle2, FileJson2, GitCompareArrows, History, RefreshCw, RotateCcw, ShieldCheck, Trash2, UploadCloud } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { Badge } from "../components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "../components/ui/card";
import { Input } from "../components/ui/input";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/ui/tabs";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "../components/ui/dialog";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "../components/ui/table";
import { cn } from "../components/ui/utils";

const diffs = [
  { id: "D-1042", type: "transport", name: "MQTT 产线 B 网关", change: "Added", scope: "1 gateway / 18 topics", source: "Protocols", user: "张工", time: "2026-06-18 11:07:12" },
  { id: "D-1043", type: "datapoint", name: "锅炉水温", change: "Modified", scope: "timeout 3000 → 5000", source: "Datapoints", user: "张工", time: "2026-06-18 11:12:44" },
  { id: "D-1044", type: "conversion rule", name: "R-204 字段映射调整", change: "Modified", scope: "temperature/raw → temp_c", source: "Conversion", user: "张工", time: "2026-06-18 11:18:05" },
];

const versions = [
  { version: "v37", time: "2026-06-18 10:22:47", user: "张工", summary: "调整 OPC UA 采样周期，修复锅炉水温映射", result: "Passed", runtime: "Active" },
  { version: "v36", time: "2026-06-17 18:46:03", user: "刘管理员", summary: "新增 MQTT 产线 A 转发规则", result: "Passed", runtime: "Archived" },
  { version: "v35", time: "2026-06-16 09:31:22", user: "张工", summary: "Modbus 连接超时策略统一为 3s", result: "Passed", runtime: "Archived" },
];

const fieldRows = [
  ["protocol", "—", "mqtt"],
  ["host", "—", "10.12.8.44"],
  ["timeout", "3000", "5000"],
  ["mapping.temperature", "raw_water_temp", "temp_c"],
];

function changeClass(change: string) {
  if (change === "Added") return "border-status-success-border bg-status-success-bg text-green-700";
  if (change === "Deleted") return "border-status-error-border bg-status-error-bg text-red-700";
  return "border-status-warning-border bg-status-warning-bg text-amber-700";
}

function StatCard({ title, primary, rows, tone = "default" }: { title: string; primary: string; rows: string[]; tone?: "default" | "success" | "draft" }) {
  return (
    <Card className={cn("border bg-card shadow-sm", tone === "success" && "border-status-success-border", tone === "draft" && "border-status-draft-border")}>
      <CardHeader className="pb-2">
        <CardTitle className="text-xs font-medium uppercase tracking-[0.08em] text-muted-foreground">{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <div className="text-xl font-semibold">{primary}</div>
        <div className="mt-2 space-y-1 font-mono text-xs text-muted-foreground">
          {rows.map((r) => <div key={r}>{r}</div>)}
        </div>
      </CardContent>
    </Card>
  );
}

export function ConfigApply() {
  const [selected, setSelected] = useState(diffs[0]);
  const [applyOpen, setApplyOpen] = useState(false);
  const [rollback, setRollback] = useState<string | null>(null);
  const [confirmText, setConfirmText] = useState("");
  const canApply = confirmText === "APPLY";
  const json = useMemo(() => JSON.stringify({ activeVersion: "v37", draftVersion: "v38", selectedDiff: selected, validation: { status: "Passed", checkedItems: 128, warnings: 1, errors: 0 } }, null, 2), [selected]);

  // ---- 真后端 /config/* ----
  interface Ver { version: string; status: string; note: string; applied_at: string; created_at: string }
  const [versionList, setVersionList] = useState<Ver[]>([]);
  const [busy, setBusy] = useState(false);

  async function loadVersions() {
    try { setVersionList(await apiGet<Ver[]>("/config/versions")); } catch { /* 忽略 */ }
  }
  useEffect(() => { loadVersions(); }, []);

  async function doValidate() {
    setBusy(true);
    try {
      const r = await apiPost<{ valid: boolean; error?: string }>("/config/validate");
      if (r.valid) toast.success("校验通过，可以发布");
      else toast.error("校验失败", { description: r.error });
    } catch (e) { toast.error(e instanceof Error ? e.message : "校验失败"); }
    finally { setBusy(false); }
  }
  async function doApply() {
    setBusy(true);
    try {
      await apiPost("/config/apply");
      setApplyOpen(false); setConfirmText("");
      toast.success("已发布，运行时已热重载生效");
      loadVersions();
    } catch (e) { toast.error(e instanceof Error ? e.message : "发布失败"); }
    finally { setBusy(false); }
  }
  async function doRollback(v: string) {
    try {
      await apiPost(`/config/versions/${v}/rollback`);
      setRollback(null);
      toast.success(`已回滚到 ${v} 并重载`);
      loadVersions();
    } catch (e) { toast.error(e instanceof Error ? e.message : "回滚失败"); }
  }
  const fmt = (s: string) => (s && /^\d+$/.test(s) ? new Date(+s * 1000).toLocaleString("zh-CN", { hour12: false }) : s || "—");

  return (
    <>
      <PageHeader
        title="配置发布"
        en="Config & Apply"
        description="对比草稿与生效配置，校验后发布到运行配置。前端权限仅用于体验，后端仍是最终权威。"
        actions={
          <>
            <Button variant="outline" size="sm" onClick={() => toast.message("配置状态已刷新") }><RefreshCw className="size-4" />刷新</Button>
            <Button variant="outline" size="sm" disabled={busy} onClick={doValidate}><ShieldCheck className="size-4" />Validate 校验</Button>
            <Button variant="destructive" size="sm" onClick={() => setApplyOpen(true)}><UploadCloud className="size-4" />Apply 发布</Button>
            <Button variant="outline" size="sm" onClick={() => toast.warning("草稿丢弃为高风险操作，此处为演示") }><Trash2 className="size-4" />丢弃草稿</Button>
            <Button variant="ghost" size="sm"><History className="size-4" />查看审计日志</Button>
          </>
        }
      />

      <main className="space-y-4 p-6">
        <section className="grid gap-4 xl:grid-cols-4 md:grid-cols-2">
          <StatCard title="当前生效版本" primary="Active Version: v37" rows={["Applied at: 2026-06-18 10:22:47", "Applied by: 张工"]} />
          <StatCard title="草稿差异" primary="Draft Diff: 3 项" rows={["Added: 1", "Modified: 2", "Deleted: 0"]} tone="draft" />
          <StatCard title="校验状态" primary="Passed" rows={["checked items: 128", "warnings: 1", "errors: 0"]} tone="success" />
          <StatCard title="发布状态" primary="Ready to apply" rows={["last apply: 2026-06-18 10:22:47", "next version: v38"]} />
        </section>

        <section className="grid gap-4 xl:grid-cols-[0.95fr_1.35fr]">
          <Card className="overflow-hidden">
            <CardHeader className="border-b py-3">
              <CardTitle className="flex items-center gap-2 text-sm"><GitCompareArrows className="size-4 text-primary" />Draft Diff List</CardTitle>
            </CardHeader>
            <CardContent className="p-0">
              <Table>
                <TableHeader><TableRow><TableHead>类型</TableHead><TableHead>对象名称</TableHead><TableHead>变更</TableHead><TableHead>影响范围</TableHead><TableHead>来源</TableHead><TableHead>修改人</TableHead><TableHead>修改时间</TableHead></TableRow></TableHeader>
                <TableBody>
                  {diffs.map((d) => (
                    <TableRow key={d.id} className={cn("cursor-pointer", selected.id === d.id && "bg-accent/70")} onClick={() => setSelected(d)}>
                      <TableCell className="font-mono text-xs">{d.type}</TableCell><TableCell className="font-medium">{d.name}</TableCell><TableCell><Badge variant="outline" className={changeClass(d.change)}>{d.change}</Badge></TableCell><TableCell className="text-muted-foreground">{d.scope}</TableCell><TableCell>{d.source}</TableCell><TableCell>{d.user}</TableCell><TableCell className="font-mono text-xs text-muted-foreground">{d.time}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </CardContent>
          </Card>

          <Card className="overflow-hidden">
            <CardHeader className="border-b py-3"><CardTitle className="text-sm">Diff Viewer · {selected.name}</CardTitle></CardHeader>
            <CardContent className="p-4">
              <Tabs defaultValue="visual">
                <TabsList><TabsTrigger value="visual">Visual Diff</TabsTrigger><TabsTrigger value="json"><FileJson2 className="size-4" />JSON Diff</TabsTrigger></TabsList>
                <TabsContent value="visual" className="mt-3">
                  <div className="grid gap-3 lg:grid-cols-2">
                    {["Active Config", "Draft Config"].map((title, i) => (
                      <div key={title} className="rounded-md border bg-muted/20">
                        <div className="border-b px-3 py-2 text-xs font-semibold text-muted-foreground">{title}</div>
                        <div className="divide-y">
                          {fieldRows.map(([k, a, b]) => <div key={k} className={cn("grid grid-cols-[150px_1fr] gap-2 px-3 py-2 font-mono text-xs", a !== b && "bg-amber-50") }><span className="text-muted-foreground">{k}</span><span>{i === 0 ? a : b}</span></div>)}
                        </div>
                      </div>
                    ))}
                  </div>
                </TabsContent>
                <TabsContent value="json" className="mt-3"><pre className="max-h-[310px] overflow-auto rounded-md bg-slate-950 p-4 font-mono text-xs leading-relaxed text-slate-100">{json}</pre></TabsContent>
              </Tabs>
            </CardContent>
          </Card>
        </section>

        <section className="grid gap-4 xl:grid-cols-[0.8fr_1.2fr]">
          <Card><CardHeader className="pb-3"><CardTitle className="text-sm">Validate Panel</CardTitle></CardHeader><CardContent><div className="flex items-center gap-2 text-sm font-medium text-green-700"><CheckCircle2 className="size-4" />校验通过，可以发布</div><div className="mt-2 font-mono text-xs text-muted-foreground">checked items: 128 · warnings: 1 · errors: 0</div><div className="mt-4 grid gap-2">{["Schema 校验", "引用完整性校验", "协议连接依赖校验", "权限与安全校验"].map((s, i) => <div key={s} className="flex items-center justify-between rounded-md border px-3 py-2 text-sm"><span>{i + 1}. {s}</span><Badge variant="outline" className="border-status-success-border bg-status-success-bg text-green-700">Passed</Badge></div>)}</div></CardContent></Card>
          <Card><CardHeader className="pb-3"><CardTitle className="text-sm">Version History</CardTitle></CardHeader><CardContent className="p-0"><Table><TableHeader><TableRow><TableHead>版本号</TableHead><TableHead>发布时间</TableHead><TableHead>发布人</TableHead><TableHead>变更摘要</TableHead><TableHead>校验结果</TableHead><TableHead>运行状态</TableHead><TableHead>操作</TableHead></TableRow></TableHeader><TableBody>{versionList.length === 0 ? <TableRow><TableCell colSpan={7} className="h-16 text-center text-muted-foreground">暂无已发布版本</TableCell></TableRow> : versionList.map((v) => <TableRow key={v.version}><TableCell className="font-mono font-semibold">v{v.version}</TableCell><TableCell className="font-mono text-xs">{fmt(v.applied_at)}</TableCell><TableCell>—</TableCell><TableCell>{v.note}</TableCell><TableCell><Badge variant="outline" className="border-status-success-border bg-status-success-bg text-green-700">Passed</Badge></TableCell><TableCell>{v.status === "active" ? "Active" : v.status === "superseded" ? "Archived" : v.status}</TableCell><TableCell><Button variant="link" size="sm" className="px-1 text-destructive" disabled={v.status === "active"} onClick={() => setRollback(v.version)}>回滚到此版本</Button></TableCell></TableRow>)}</TableBody></Table></CardContent></Card>
        </section>
      </main>

      <Dialog open={applyOpen} onOpenChange={setApplyOpen}><DialogContent><DialogHeader><DialogTitle>确认发布草稿配置？</DialogTitle><DialogDescription>此操作会将 draft config 发布为新的运行配置，后端会再次执行权限与版本校验。</DialogDescription></DialogHeader><div className="grid gap-2 rounded-md border bg-muted/30 p-3 text-sm"><div>当前版本：v37</div><div>新版本：v38</div><div>差异：3 项</div><div>影响对象：1 transport, 1 datapoint, 1 conversion rule</div><div>校验状态：Passed</div><div>操作者：张工</div></div><Input value={confirmText} onChange={(e) => setConfirmText(e.target.value)} placeholder="请输入 APPLY 以确认发布" className="font-mono" /><DialogFooter><Button variant="outline" onClick={() => setApplyOpen(false)}>取消</Button><Button variant="destructive" disabled={!canApply || busy} onClick={doApply}>确认发布</Button></DialogFooter></DialogContent></Dialog>

      <Dialog open={!!rollback} onOpenChange={(o) => !o && setRollback(null)}><DialogContent><DialogHeader><DialogTitle>确认回滚到版本 {rollback}？</DialogTitle><DialogDescription>回滚会创建一个新的 draft config，仍需 Validate 和 Apply 后才会生效。</DialogDescription></DialogHeader><DialogFooter><Button variant="outline" onClick={() => setRollback(null)}>取消</Button><Button onClick={() => rollback && doRollback(rollback)}><RotateCcw className="size-4" />确认回滚并重载</Button></DialogFooter></DialogContent></Dialog>
    </>
  );
}
