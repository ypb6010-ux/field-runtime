import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { Plus, RefreshCw, Pencil, Trash2, Loader2, AlertCircle } from "lucide-react";
import { apiGet, apiPost, apiPut, apiDelete, isValidResourceId } from "../api";
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
import { Textarea } from "../components/ui/textarea";

// 后端 /datapoints 行（rowToJson 返回字符串字段）
interface DpRow {
  id: string;
  transport_id: string;
  reg_table: string;
  addr: string;
  type: string;
  word_order: string;
  scale: string;
  codec_id: string;
  kind: string;
  enabled: string;
}
interface TpRow { id: string; name?: string; kind: string }
interface CodecRow {
  id: string;
  kind: string;
  params_json: string;
}

const TYPES = ["U16", "S16", "U32", "S32", "U64", "S64", "F32", "F64", "EnumU16"];
const TABLES = ["HR", "IR"];
const MULTI_REGISTER_TYPES = new Set(["U32", "S32", "F32", "U64", "S64", "F64"]);

interface Form {
  id: string; transport_id: string; reg_table: string; addr: number;
  type: string; word_order: string; scale: number; codec_id: string; kind: string; enabled: boolean;
}
const EMPTY_FORM: Form = { id: "", transport_id: "", reg_table: "HR", addr: 0, type: "U16", word_order: "hi_lo", scale: 1, codec_id: "", kind: "Status", enabled: true };

export function Datapoints({ canWrite = true, onDraftIncrement }: { canWrite?: boolean; onDraftIncrement?: () => void }) {
  const [rows, setRows] = useState<DpRow[]>([]);
  const [transports, setTransports] = useState<TpRow[]>([]);
  const [codecs, setCodecs] = useState<CodecRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [kw, setKw] = useState("");
  const [tpFilter, setTpFilter] = useState("all");
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit" }>({ open: false, mode: "create" });
  const [form, setForm] = useState<Form>(EMPTY_FORM);
  const [del, setDel] = useState<DpRow | null>(null);
  const [codecOpen, setCodecOpen] = useState(false);
  const [codecForm, setCodecForm] = useState<{
    mode: "create" | "edit";
    id: string;
    mapText: string;
  } | null>(null);
  const [codecDel, setCodecDel] = useState<CodecRow | null>(null);

  async function load() {
    setLoading(true); setError(null);
    try {
      const [dps, tps, codecRows] = await Promise.all([
        apiGet<DpRow[]>("/datapoints"),
        apiGet<TpRow[]>("/transports"),
        apiGet<CodecRow[]>("/codecs"),
      ]);
      setRows(dps); setTransports(tps); setCodecs(codecRows);
    } catch (e) {
      setError(e instanceof Error ? e.message : "加载失败");
    } finally { setLoading(false); }
  }
  useEffect(() => { load(); }, []);

  const filtered = useMemo(() => rows.filter((r) => {
    if (tpFilter !== "all" && r.transport_id !== tpFilter) return false;
    if (kw && !`${r.id} ${r.addr}`.toLowerCase().includes(kw.toLowerCase())) return false;
    return true;
  }), [rows, kw, tpFilter]);

  function openCreate() { setForm({ ...EMPTY_FORM, transport_id: transports[0]?.id ?? "" }); setDrawer({ open: true, mode: "create" }); }
  function openEdit(r: DpRow) {
    setForm({ id: r.id, transport_id: r.transport_id, reg_table: r.reg_table, addr: +r.addr || 0, type: r.type, word_order: r.word_order === "hi_lo" ? "ABCD" : r.word_order === "lo_hi" ? "CDAB" : r.word_order || "ABCD", scale: +r.scale || 1, codec_id: r.codec_id || "", kind: "Status", enabled: r.enabled === "1" });
    setDrawer({ open: true, mode: "edit" });
  }

  async function save() {
    if (!isValidResourceId(form.id)) {
      toast.error("点位 ID 须为 1–128 字符，且不可含空白或 /\\?#%");
      return;
    }
    const words = ["U64", "S64", "F64"].includes(form.type)
      ? 4
      : MULTI_REGISTER_TYPES.has(form.type)
        ? 2
        : 1;
    if (!Number.isInteger(form.addr) || form.addr < 0 || form.addr + words > 65536) {
      toast.error("地址范围必须完整落在 0..65535");
      return;
    }
    if (!Number.isFinite(form.scale) || form.scale === 0) {
      toast.error("scale 必须是非零有效数字");
      return;
    }
    if (form.type === "EnumU16" && !form.codec_id) {
      toast.error("EnumU16 必须选择枚举编解码器");
      return;
    }
    const body = { ...form, enabled: form.enabled ? 1 : 0 };
    try {
      if (drawer.mode === "create") await apiPost("/datapoints", body);
      else await apiPut(`/datapoints/${encodeURIComponent(form.id)}`, body);
      setDrawer((d) => ({ ...d, open: false }));
      onDraftIncrement?.();
      toast.success("已保存，需到 Config & Apply 发布后生效");
      load();
    } catch (e) { toast.error(e instanceof Error ? e.message : "保存失败"); }
  }
  async function doDelete() {
    if (!del) return;
    try { await apiDelete(`/datapoints/${encodeURIComponent(del.id)}`); setDel(null); onDraftIncrement?.(); toast.success("已删除"); load(); }
    catch (e) { toast.error(e instanceof Error ? e.message : "删除失败"); }
  }

  function editCodec(codec: CodecRow) {
    let map: unknown = {};
    try {
      const params = JSON.parse(codec.params_json) as Record<string, unknown>;
      map = params.map ?? params;
    } catch {
      map = {};
    }
    setCodecForm({
      mode: "edit",
      id: codec.id,
      mapText: JSON.stringify(map, null, 2),
    });
  }

  async function saveCodec() {
    if (!codecForm) return;
    if (!isValidResourceId(codecForm.id)) {
      toast.error("字典 ID 须为 1–128 字符，且不可含空白或 /\\?#%");
      return;
    }
    let map: unknown;
    try {
      map = JSON.parse(codecForm.mapText);
    } catch {
      toast.error("枚举映射必须是有效 JSON");
      return;
    }
    if (!map || typeof map !== "object" || Array.isArray(map)
        || Object.keys(map).length === 0) {
      toast.error("枚举映射必须是非空对象");
      return;
    }
    const invalid = Object.entries(map).some(([key, value]) => {
      const numeric = Number(key);
      return !/^\d+$/.test(key) || !Number.isInteger(numeric)
        || numeric < 0 || numeric > 65535 || typeof value !== "string";
    });
    if (invalid) {
      toast.error("键必须为 0..65535 的十进制整数，值必须为字符串");
      return;
    }
    const body = {
      id: codecForm.id,
      kind: "enum_u16",
      params_json: { map },
      script_path: "",
    };
    try {
      if (codecForm.mode === "create") await apiPost("/codecs", body);
      else await apiPut(`/codecs/${encodeURIComponent(codecForm.id)}`, body);
      setCodecForm(null);
      onDraftIncrement?.();
      toast.success("枚举字典已保存到配置草稿");
      await load();
    } catch (codecError) {
      toast.error(codecError instanceof Error ? codecError.message : "字典保存失败");
    }
  }

  async function deleteCodec() {
    if (!codecDel) return;
    try {
      await apiDelete(`/codecs/${encodeURIComponent(codecDel.id)}`);
      setCodecDel(null);
      onDraftIncrement?.();
      toast.success("枚举字典已删除");
      await load();
    } catch (codecError) {
      toast.error(codecError instanceof Error ? codecError.message : "字典删除失败");
    }
  }

  const tpName = (id: string) => transports.find((t) => t.id === id)?.name || transports.find((t) => t.id === id)?.id || id;

  return (
    <>
      <PageHeader
        title="采集点" en="Datapoints"
        description="管理设备点位、地址、数据类型与编解码配置（实时来自后端 /datapoints）。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}><RefreshCw className="size-3.5" />刷新</Button>
            <Button variant="outline" size="sm" onClick={() => setCodecOpen(true)}>枚举字典</Button>
            <PermissionButton allowed={canWrite} onAction={openCreate} size="sm"><Plus className="size-3.5" />新增采集点</PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
          <Input className="h-8 w-64" placeholder="搜索点位 ID / 地址" value={kw} onChange={(e) => setKw(e.target.value)} />
          <Select value={tpFilter} onValueChange={setTpFilter}>
            <SelectTrigger className="h-8 w-52"><SelectValue placeholder="Transport" /></SelectTrigger>
            <SelectContent><SelectItem value="all">全部 Transport</SelectItem>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{tpName(t.id)}</SelectItem>)}</SelectContent>
          </Select>
          <span className="ml-auto text-xs text-muted-foreground">共 {filtered.length} 个点位</span>
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
                  <TableHead>点位 ID</TableHead><TableHead>Transport</TableHead><TableHead>表</TableHead><TableHead>地址</TableHead>
                  <TableHead>类型</TableHead><TableHead>scale</TableHead><TableHead>编解码</TableHead><TableHead>状态</TableHead>
                  <TableHead className="text-right">操作</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {filtered.map((r) => (
                  <TableRow key={r.id}>
                    <TableCell className="font-medium">{r.id}</TableCell>
                    <TableCell className="text-muted-foreground">{tpName(r.transport_id)}</TableCell>
                    <TableCell>{r.reg_table}</TableCell>
                    <TableCell className="font-mono text-xs">{r.addr}</TableCell>
                    <TableCell>{r.type}</TableCell>
                    <TableCell>{r.scale}</TableCell>
                    <TableCell className="text-xs">{r.codec_id || "-"}</TableCell>
                    <TableCell><Badge variant="outline" className={r.enabled === "1" ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-muted text-muted-foreground border-border"}>{r.enabled === "1" ? "启用" : "禁用"}</Badge></TableCell>
                    <TableCell className="text-right">
                      <div className="flex justify-end gap-1">
                        <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => openEdit(r)}><Pencil className="size-3.5" /></Button>
                        <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(r)}><Trash2 className="size-3.5" /></Button>
                      </div>
                    </TableCell>
                  </TableRow>
                ))}
                {filtered.length === 0 && <TableRow><TableCell colSpan={9} className="h-24 text-center text-muted-foreground">暂无采集点</TableCell></TableRow>}
              </TableBody>
            </Table>
          </div>
        )}
      </div>

      <Sheet open={drawer.open} onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}>
        <SheetContent className="flex w-full flex-col sm:max-w-lg">
          <SheetHeader>
            <SheetTitle>{drawer.mode === "create" ? "新增采集点" : `编辑采集点 · ${form.id}`}</SheetTitle>
            <SheetDescription>保存后写入草稿，需到 Config &amp; Apply 发布后生效。</SheetDescription>
          </SheetHeader>
          <div className="flex-1 space-y-3 overflow-y-auto px-4">
            <Field label="点位 ID"><Input value={form.id} disabled={drawer.mode === "edit"} onChange={(e) => setForm((f) => ({ ...f, id: e.target.value }))} placeholder="如 sim.temperature" /></Field>
            <Field label="Transport">
              <Select value={form.transport_id} onValueChange={(v) => setForm((f) => ({ ...f, transport_id: v }))}>
                <SelectTrigger><SelectValue placeholder="选择" /></SelectTrigger>
                <SelectContent>{transports.map((t) => <SelectItem key={t.id} value={t.id}>{tpName(t.id)}</SelectItem>)}</SelectContent>
              </Select>
            </Field>
            <Field label="寄存器表">
              <Select value={form.reg_table} onValueChange={(v) => setForm((f) => ({ ...f, reg_table: v }))}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>{TABLES.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
              </Select>
            </Field>
            <Field label="地址 addr"><Input type="number" value={form.addr} onChange={(e) => setForm((f) => ({ ...f, addr: +e.target.value }))} /></Field>
            <Field label="数据类型">
              <Select value={form.type} onValueChange={(value) => setForm((current) => ({
                ...current,
                type: value,
                word_order: MULTI_REGISTER_TYPES.has(value) ? "ABCD" : current.word_order,
                codec_id: value === "EnumU16" ? current.codec_id : "",
              }))}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>{TYPES.map((t) => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent>
              </Select>
            </Field>
            {MULTI_REGISTER_TYPES.has(form.type) && (
              <Field label="字序 word_order">
                <Select value={form.word_order} onValueChange={(value) => setForm((current) => ({ ...current, word_order: value }))}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent>
                    {["ABCD", "CDAB", "BADC", "DCBA"].map((order) => <SelectItem key={order} value={order}>{order}</SelectItem>)}
                  </SelectContent>
                </Select>
              </Field>
            )}
            <Field label="scale"><Input type="number" step="0.001" value={form.scale} onChange={(e) => setForm((f) => ({ ...f, scale: +e.target.value }))} /></Field>
            {form.type === "EnumU16" && (
              <Field label="枚举 codec">
                <Select value={form.codec_id} onValueChange={(value) => setForm((current) => ({ ...current, codec_id: value }))}>
                  <SelectTrigger><SelectValue placeholder="选择 enum_u16 codec" /></SelectTrigger>
                  <SelectContent>{codecs.filter((codec) => codec.kind === "enum_u16").map((codec) => <SelectItem key={codec.id} value={codec.id}>{codec.id}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
            )}
            <Field label="启用"><Switch checked={form.enabled} onCheckedChange={(v) => setForm((f) => ({ ...f, enabled: v }))} /></Field>
          </div>
          <SheetFooter className="flex-row justify-end gap-2">
            <Button variant="outline" onClick={() => setDrawer((d) => ({ ...d, open: false }))}>取消</Button>
            <Button onClick={save} disabled={!form.id || !form.transport_id}>保存草稿</Button>
          </SheetFooter>
        </SheetContent>
      </Sheet>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除采集点？</DialogTitle>
            <DialogDescription>
              将删除「{del?.id}」。该操作会记入未生效配置；若仍被转换规则引用，后端会拒绝删除。
            </DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setDel(null)}>取消</Button>
            <Button variant="destructive" onClick={doDelete}>确认删除</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={codecOpen} onOpenChange={(open) => {
        setCodecOpen(open);
        if (!open) setCodecForm(null);
      }}>
        <DialogContent className="sm:max-w-2xl">
          <DialogHeader>
            <DialogTitle>枚举字典</DialogTitle>
            <DialogDescription>
              管理 EnumU16 点位使用的数值到文本映射；保存后仍需发布配置。
            </DialogDescription>
          </DialogHeader>
          {codecForm ? (
            <div className="space-y-3">
              <Field label="字典 ID">
                <Input
                  value={codecForm.id}
                  disabled={codecForm.mode === "edit"}
                  onChange={(event) => setCodecForm((current) =>
                    current ? { ...current, id: event.target.value } : current)}
                  placeholder="如 machine_state"
                />
              </Field>
              <Field label="枚举映射">
                <Textarea
                  className="min-h-52 font-mono text-xs"
                  value={codecForm.mapText}
                  onChange={(event) => setCodecForm((current) =>
                    current ? { ...current, mapText: event.target.value } : current)}
                  placeholder={'{\n  "0": "停止",\n  "1": "运行"\n}'}
                />
              </Field>
            </div>
          ) : (
            <div className="max-h-96 overflow-auto rounded-md border">
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>字典 ID</TableHead>
                    <TableHead>类型</TableHead>
                    <TableHead className="text-right">操作</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {codecs.map((codec) => (
                    <TableRow key={codec.id}>
                      <TableCell className="font-mono text-xs">{codec.id}</TableCell>
                      <TableCell>{codec.kind}</TableCell>
                      <TableCell className="text-right">
                        <Button variant="ghost" size="icon" disabled={!canWrite} onClick={() => editCodec(codec)}>
                          <Pencil className="size-3.5" />
                        </Button>
                        <Button variant="ghost" size="icon" className="text-red-600" disabled={!canWrite} onClick={() => setCodecDel(codec)}>
                          <Trash2 className="size-3.5" />
                        </Button>
                      </TableCell>
                    </TableRow>
                  ))}
                  {codecs.length === 0 && (
                    <TableRow>
                      <TableCell colSpan={3} className="h-24 text-center text-muted-foreground">
                        暂无枚举字典
                      </TableCell>
                    </TableRow>
                  )}
                </TableBody>
              </Table>
            </div>
          )}
          <DialogFooter>
            {codecForm ? (
              <>
                <Button variant="outline" onClick={() => setCodecForm(null)}>返回列表</Button>
                <Button onClick={saveCodec} disabled={!codecForm.id.trim()}>保存字典</Button>
              </>
            ) : (
              <>
                <Button variant="outline" onClick={() => setCodecOpen(false)}>关闭</Button>
                <Button
                  disabled={!canWrite}
                  onClick={() => setCodecForm({
                    mode: "create",
                    id: "",
                    mapText: '{\n  "0": "停止",\n  "1": "运行"\n}',
                  })}
                >
                  <Plus className="size-3.5" />新增字典
                </Button>
              </>
            )}
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={!!codecDel} onOpenChange={(open) => !open && setCodecDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除枚举字典？</DialogTitle>
            <DialogDescription>
              将删除「{codecDel?.id}」。仍被点位引用的字典会被后端拒绝删除。
            </DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setCodecDel(null)}>取消</Button>
            <Button variant="destructive" onClick={deleteCodec}>确认删除</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
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
