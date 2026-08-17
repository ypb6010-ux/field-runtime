import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { Activity, Plus, RefreshCw, Save, Send, Trash2, Waypoints } from "lucide-react";
import { apiGet, apiPost, apiPut } from "../api";
import { PageHeader } from "../components/PageHeader";
import { Badge } from "../components/ui/badge";
import { Button } from "../components/ui/button";
import { Checkbox } from "../components/ui/checkbox";
import { Input } from "../components/ui/input";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/ui/tabs";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "../components/ui/table";

type EntityKey = "drivers" | "servers" | "bridges" | "devices" | "routes" | "targets" | "actors" | "policies";
type Row = Record<string, unknown>;
interface ControlConfig {
  drivers: Row[];
  servers: Row[];
  bridges: Row[];
  devices: Row[];
  routes: Row[];
  targets: Row[];
  actors: Row[];
  policies: Row[];
  mqtt: Row;
}

interface FieldSpec {
  key: string;
  label: string;
  type?: "boolean" | "number";
  width?: string;
}

const SPECS: Record<EntityKey, { label: string; fields: FieldSpec[]; defaults: Row }> = {
  drivers: {
    label: "驱动",
    fields: [
      { key: "id", label: "ID" }, { key: "library", label: "预安装库", width: "min-w-64" },
      { key: "config", label: "适配器配置", width: "min-w-56" }, { key: "enabled", label: "启用", type: "boolean" },
    ],
    defaults: { id: "", library: "", config: "{}", enabled: true },
  },
  servers: {
    label: "操作箱入口",
    fields: [
      { key: "id", label: "ID" }, { key: "name", label: "名称" },
      { key: "listen_address", label: "监听地址" }, { key: "listen_port", label: "端口", type: "number" },
      { key: "max_clients", label: "最大连接", type: "number" }, { key: "range_start", label: "HR 起点", type: "number" },
      { key: "range_count", label: "HR 数量", type: "number" }, { key: "enabled", label: "启用", type: "boolean" },
    ],
    defaults: { id: "", name: "", listen_address: "0.0.0.0", listen_port: 502, max_clients: 4, range_start: 0, range_count: 64, enabled: true },
  },
  bridges: {
    label: "桥接",
    fields: [
      { key: "id", label: "ID" }, { key: "server_id", label: "操作箱入口" }, { key: "plc_transport_id", label: "PLC Transport" },
      { key: "offset", label: "偏移", type: "number" }, { key: "write_start", label: "写起点", type: "number" },
      { key: "write_count", label: "写数量", type: "number" }, { key: "mirror_start", label: "回显起点", type: "number" },
      { key: "mirror_count", label: "回显数量", type: "number" }, { key: "mirror_policy", label: "回显策略" },
      { key: "mirror_period_ms", label: "回显周期 ms", type: "number" },
    ],
    defaults: { id: "", server_id: "", plc_transport_id: "", offset: 0, write_start: 20, write_count: 1, mirror_start: 0, mirror_count: 8, mirror_policy: "AfterPoll", mirror_period_ms: 0 },
  },
  devices: {
    label: "设备",
    fields: [{ key: "id", label: "ID" }, { key: "name", label: "名称" }, { key: "driver_id", label: "默认驱动" }],
    defaults: { id: "", name: "", driver_id: "" },
  },
  routes: {
    label: "写路由",
    fields: [
      { key: "id", label: "ID" }, { key: "device_id", label: "设备" }, { key: "protocol", label: "协议" },
      { key: "transport_id", label: "Transport" }, { key: "driver_id", label: "Driver" },
      { key: "writable", label: "可写", type: "boolean" }, { key: "active", label: "活动", type: "boolean" },
    ],
    defaults: { id: "", device_id: "", protocol: "modbus", transport_id: "", driver_id: "", writable: true, active: false },
  },
  targets: {
    label: "控制目标",
    fields: [
      { key: "id", label: "ID" }, { key: "device_id", label: "设备" }, { key: "route_id", label: "固定路由" },
      { key: "protocol", label: "协议" }, { key: "endpoint", label: "端点" }, { key: "resource", label: "资源" },
      { key: "selector", label: "JSON Pointer" }, { key: "offset", label: "偏移", type: "number" },
      { key: "width", label: "宽度", type: "number" }, { key: "mask", label: "掩码", type: "number" },
    ],
    defaults: { id: "", device_id: "", route_id: "", protocol: "modbus", endpoint: "", resource: "HR", selector: "", offset: 0, width: 1, mask: -1 },
  },
  actors: {
    label: "参与者",
    fields: [
      { key: "id", label: "ID" }, { key: "channel", label: "通道" }, { key: "client_id", label: "客户端 ID" },
      { key: "source_address", label: "来源地址" }, { key: "role", label: "角色" },
      { key: "priority", label: "优先级", type: "number" }, { key: "enabled", label: "启用", type: "boolean" },
    ],
    defaults: { id: "", channel: "web", client_id: "", source_address: "", role: "", priority: 0, enabled: true },
  },
  policies: {
    label: "仲裁策略",
    fields: [
      { key: "id", label: "ID" }, { key: "target_id", label: "目标" }, { key: "mode", label: "模式" },
      { key: "lease_ms", label: "租约 ms", type: "number" }, { key: "min_priority", label: "最低优先级", type: "number" },
    ],
    defaults: { id: "", target_id: "", mode: "open", lease_ms: 0, min_priority: 0 },
  },
};

const EMPTY: ControlConfig = {
  drivers: [], servers: [], bridges: [], devices: [], routes: [], targets: [], actors: [], policies: [],
  mqtt: { enable: false, host: "127.0.0.1", port: 1883, client_id: "field_gateway", topic_prefix: "field", command_topic_prefix: "field/control", qos: 1, publish_interval_ms: 1000 },
};

interface RuntimeState {
  running: boolean;
  drivers: Row[];
  routes: Row[];
  leases: Row[];
  data: Row[];
}

function boolValue(value: unknown) {
  return value === true || value === 1 || value === "1";
}

export function ControlCenter({ canWrite = true, onChanged }: { canWrite?: boolean; onChanged?: () => void }) {
  const [config, setConfig] = useState<ControlConfig>(EMPTY);
  const [runtime, setRuntime] = useState<RuntimeState>({ running: false, drivers: [], routes: [], leases: [], data: [] });
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [targetId, setTargetId] = useState("");
  const [payload, setPayload] = useState("");

  async function loadConfig() {
    setLoading(true);
    try {
      const value = await apiGet<ControlConfig>("/control/config");
      setConfig(value);
    } catch (error) {
      toast.error(error instanceof Error ? error.message : "控制配置加载失败");
    } finally {
      setLoading(false);
    }
  }

  async function loadRuntime() {
    try { setRuntime(await apiGet<RuntimeState>("/control/runtime")); }
    catch { /* Runtime status is refreshed again while this page is open. */ }
  }

  useEffect(() => {
    loadConfig();
    loadRuntime();
    const timer = window.setInterval(loadRuntime, 3000);
    return () => window.clearInterval(timer);
  }, []);

  const targetOptions = useMemo(() => config.targets.map((row) => String(row.id ?? "")).filter(Boolean), [config.targets]);

  function update(key: EntityKey, index: number, field: FieldSpec, value: string | boolean) {
    setConfig((current) => ({
      ...current,
      [key]: current[key].map((row, rowIndex) => rowIndex === index
        ? { ...row, [field.key]: field.type === "number" ? Number(value) : value }
        : row),
    }));
  }

  function updateMqtt(key: string, value: string | boolean | number) {
    setConfig((current) => ({ ...current, mqtt: { ...current.mqtt, [key]: value } }));
  }

  function add(key: EntityKey) {
    setConfig((current) => ({ ...current, [key]: [...current[key], { ...SPECS[key].defaults }] }));
  }

  function remove(key: EntityKey, index: number) {
    setConfig((current) => ({ ...current, [key]: current[key].filter((_, rowIndex) => rowIndex !== index) }));
  }

  async function save() {
    setSaving(true);
    try {
      setConfig(await apiPut<ControlConfig>("/control/config", config));
      onChanged?.();
      toast.success("控制配置已保存到草稿");
    } catch (error) {
      toast.error(error instanceof Error ? error.message : "保存失败");
    } finally { setSaving(false); }
  }

  async function activate(row: Row) {
    try {
      await apiPost("/control/routes/activate", { device_id: row.device_id, route_id: row.id });
      onChanged?.();
      await loadRuntime();
      toast.success("活动写路由已切换");
    } catch (error) { toast.error(error instanceof Error ? error.message : "切换失败"); }
  }

  async function writeTarget() {
    const bytes = payload.split(/[\s,]+/).filter(Boolean).map(Number);
    if (!targetId || bytes.length === 0 || bytes.some((value) => !Number.isInteger(value) || value < 0 || value > 255)) {
      toast.error("请选择目标并输入 0..255 字节");
      return;
    }
    try {
      await apiPost("/control/write", { target_id: targetId, payload: bytes });
      toast.success("控制请求已提交");
    } catch (error) { toast.error(error instanceof Error ? error.message : "写入失败"); }
  }

  return (
    <div className="flex h-full flex-col bg-background">
      <PageHeader
        title="设备与控制"
        en="Devices & Control"
        actions={(
          <>
            <Button variant="outline" size="sm" onClick={() => { loadConfig(); loadRuntime(); }} disabled={loading}>
              <RefreshCw className={`size-4 ${loading ? "animate-spin" : ""}`} />刷新
            </Button>
            <Button size="sm" onClick={save} disabled={!canWrite || saving}>
              <Save className="size-4" />保存草稿
            </Button>
          </>
        )}
      />
      <Tabs defaultValue="configuration" className="min-h-0 flex-1 px-6 py-4">
        <TabsList>
          <TabsTrigger value="configuration"><Waypoints className="size-4" />配置</TabsTrigger>
          <TabsTrigger value="runtime"><Activity className="size-4" />运行态</TabsTrigger>
        </TabsList>
        <TabsContent value="configuration" className="mt-4">
          <div className="mb-4 grid gap-3 border px-4 py-3 md:grid-cols-4 xl:grid-cols-8">
            <label className="flex items-center gap-2 text-sm"><Checkbox checked={boolValue(config.mqtt.enable)} onCheckedChange={(value) => updateMqtt("enable", value === true)} disabled={!canWrite} />MQTT</label>
            {[
              ["host", "Broker", "text"], ["port", "端口", "number"], ["client_id", "Client ID", "text"],
              ["topic_prefix", "发布前缀", "text"], ["command_topic_prefix", "命令前缀", "text"],
              ["qos", "QoS", "number"], ["publish_interval_ms", "发布周期 ms", "number"],
            ].map(([key, label, type]) => <label key={key} className="text-xs text-muted-foreground">{label}<Input className="mt-1 h-8" type={type} value={String(config.mqtt[key] ?? "")} onChange={(event) => updateMqtt(key, type === "number" ? Number(event.target.value) : event.target.value)} disabled={!canWrite} /></label>)}
          </div>
          <Tabs defaultValue="devices">
            <TabsList className="flex-wrap">
              {(Object.keys(SPECS) as EntityKey[]).map((key) => (
                <TabsTrigger key={key} value={key}>{SPECS[key].label} <Badge variant="secondary">{config[key].length}</Badge></TabsTrigger>
              ))}
            </TabsList>
            {(Object.keys(SPECS) as EntityKey[]).map((key) => (
              <TabsContent key={key} value={key} className="mt-3 border">
                <div className="flex items-center justify-end border-b px-3 py-2">
                  <Button variant="outline" size="sm" onClick={() => add(key)} disabled={!canWrite}><Plus className="size-4" />新增</Button>
                </div>
                <Table>
                  <TableHeader><TableRow>
                    {SPECS[key].fields.map((field) => <TableHead key={field.key} className={field.width}>{field.label}</TableHead>)}
                    <TableHead className="w-12" />
                  </TableRow></TableHeader>
                  <TableBody>
                    {config[key].map((row, index) => (
                      <TableRow key={`${key}-${index}`}>
                        {SPECS[key].fields.map((field) => (
                          <TableCell key={field.key} className="py-1.5">
                            {field.type === "boolean" ? (
                              <Checkbox checked={boolValue(row[field.key])} onCheckedChange={(value) => update(key, index, field, value === true)} disabled={!canWrite} />
                            ) : (
                              <Input className="h-8 min-w-28" type={field.type === "number" ? "number" : "text"}
                                value={String(row[field.key] ?? "")} onChange={(event) => update(key, index, field, event.target.value)} disabled={!canWrite} />
                            )}
                          </TableCell>
                        ))}
                        <TableCell><Button variant="ghost" size="icon" title="删除" onClick={() => remove(key, index)} disabled={!canWrite}><Trash2 className="size-4" /></Button></TableCell>
                      </TableRow>
                    ))}
                    {config[key].length === 0 && <TableRow><TableCell colSpan={SPECS[key].fields.length + 1} className="h-24 text-center text-muted-foreground">暂无配置</TableCell></TableRow>}
                  </TableBody>
                </Table>
              </TabsContent>
            ))}
          </Tabs>
        </TabsContent>
        <TabsContent value="runtime" className="mt-4 space-y-4">
          <div className="flex flex-wrap items-end gap-3 border px-4 py-3">
            <div className="min-w-56"><div className="mb-1 text-xs text-muted-foreground">控制目标</div>
              <select className="h-9 w-full rounded-md border bg-background px-3 text-sm" value={targetId} onChange={(event) => setTargetId(event.target.value)}>
                <option value="">请选择</option>{targetOptions.map((id) => <option key={id}>{id}</option>)}
              </select>
            </div>
            <div className="min-w-72 flex-1"><div className="mb-1 text-xs text-muted-foreground">字节载荷</div><Input value={payload} onChange={(event) => setPayload(event.target.value)} placeholder="00, 01, 255" /></div>
            <Button onClick={writeTarget} disabled={!canWrite}><Send className="size-4" />发送</Button>
          </div>
          <div className="border">
            <Table><TableHeader><TableRow><TableHead>设备</TableHead><TableHead>路由</TableHead><TableHead>协议</TableHead><TableHead>状态</TableHead><TableHead className="w-24" /></TableRow></TableHeader>
              <TableBody>{runtime.routes.map((row, index) => <TableRow key={`route-${index}`}><TableCell>{String(row.device_id ?? "")}</TableCell><TableCell>{String(row.id ?? "")}</TableCell><TableCell>{String(row.protocol ?? "")}</TableCell><TableCell><Badge variant={boolValue(row.active) ? "default" : "secondary"}>{boolValue(row.active) ? "活动" : "备用"}</Badge></TableCell><TableCell><Button size="sm" variant="outline" disabled={!canWrite || boolValue(row.active)} onClick={() => activate(row)}>切换</Button></TableCell></TableRow>)}</TableBody>
            </Table>
          </div>
          <div className="grid gap-4 xl:grid-cols-2">
            <RuntimeTable title="驱动" rows={runtime.drivers} columns={["id", "state", "error"]} />
            <RuntimeTable title="租约" rows={runtime.leases} columns={["target_id", "actor_id", "priority", "expires_at"]} />
          </div>
          <RuntimeTable title="数据回显" rows={runtime.data} columns={["device_id", "target_id", "payload", "ts"]} />
        </TabsContent>
      </Tabs>
    </div>
  );
}

function RuntimeTable({ title, rows, columns }: { title: string; rows: Row[]; columns: string[] }) {
  return <div className="border"><div className="border-b px-4 py-2 text-sm font-medium">{title}</div><Table><TableHeader><TableRow>{columns.map((column) => <TableHead key={column}>{column}</TableHead>)}</TableRow></TableHeader><TableBody>{rows.map((row, index) => <TableRow key={`${title}-${index}`}>{columns.map((column) => <TableCell key={column}>{Array.isArray(row[column]) ? (row[column] as unknown[]).join(", ") : String(row[column] ?? "-")}</TableCell>)}</TableRow>)}{rows.length === 0 && <TableRow><TableCell colSpan={columns.length} className="h-20 text-center text-muted-foreground">暂无运行数据</TableCell></TableRow>}</TableBody></Table></div>;
}
