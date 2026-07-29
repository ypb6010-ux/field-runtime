/**
 * Transport（协议连接）API 适配层。
 *
 * 后端接口统一经 /api/v1，配置表单 schema 来自 GET /transports/kinds。
 * rowToJson 返回字符串字段，因此这里负责把 enabled、JSON 列等转换成前端模型。
 */

import { apiDelete, apiGet, apiPost, apiPut } from "./api";

// ---- 状态 ----
export type TransportStatus = "online" | "offline" | "reconnecting" | "error" | "disabled";

export const STATUS_META: Record<
  TransportStatus,
  { label: string; tone: "success" | "warning" | "error" | "disabled" }
> = {
  online: { label: "在线", tone: "success" },
  offline: { label: "离线", tone: "disabled" },
  reconnecting: { label: "重连中", tone: "warning" },
  error: { label: "错误", tone: "error" },
  disabled: { label: "禁用", tone: "disabled" },
};

// ---- JSON Schema 字段模型（后端返回的简化结构）----
export type SchemaFieldType =
  | "string"
  | "number"
  | "boolean"
  | "enum"
  | "secret"
  | "text"
  | "object"
  | "array"
  | "unsupported";

export interface SchemaFieldDef {
  name: string;
  title: string;
  type: SchemaFieldType;
  required?: boolean;
  default?: string | number | boolean;
  description?: string;
  placeholder?: string;
  unit?: string;
  minimum?: number;
  maximum?: number;
  options?: { label: string; value: string }[];
  advanced?: boolean;
  rawType?: string;
}

export interface KindSchema {
  kind: string;
  label: string;
  version: string;
  fields: SchemaFieldDef[];
}

interface KindApiRow {
  id: string;
  label: string;
  params: Record<string, {
    type?: string;
    label?: string;
    default?: string | number | boolean;
    required?: boolean;
    description?: string;
    advanced?: boolean;
    minimum?: number;
    maximum?: number;
  }>;
}

interface TransportRow {
  id: string;
  name: string;
  kind: string;
  enabled: string;
  params_json?: string | null;
  scheduler_json?: string | null;
  created_at?: string | null;
  updated_at?: string | null;
}
interface RuntimeTransportRow {
  id: string;
  state: "connected" | "connecting" | "disconnected" | "error";
}
interface DatapointRow {
  transport_id: string;
}

// ---- 协议连接记录 ----
export interface Transport {
  id: string;
  name: string;
  kind: string;
  endpoint: string;
  status: TransportStatus;
  enabled: boolean;
  pointCount: number;
  updatedAt: string | null;
  config: Record<string, string | number | boolean>;
  scheduler: Record<string, unknown>;
}

export interface TransportPayload {
  id: string;
  name: string;
  kind: string;
  enabled: boolean;
  params_json: Record<string, string | number | boolean>;
  scheduler_json?: Record<string, unknown>;
}

let cachedKinds: KindSchema[] = [];

function parseJsonObject(s: string | null | undefined): Record<string, string | number | boolean> {
  if (!s) return {};
  try {
    const v = JSON.parse(s) as unknown;
    if (!v || typeof v !== "object" || Array.isArray(v)) return {};
    const out: Record<string, string | number | boolean> = {};
    Object.entries(v as Record<string, unknown>).forEach(([k, value]) => {
      if (["string", "number", "boolean"].includes(typeof value)) {
        out[k] = value as string | number | boolean;
      }
    });
    return out;
  } catch {
    return {};
  }
}

function schemaType(raw: string | undefined): SchemaFieldType {
  switch ((raw ?? "string").toLowerCase()) {
    case "int":
    case "integer":
    case "float":
    case "double":
    case "number":
      return "number";
    case "bool":
    case "boolean":
      return "boolean";
    case "text":
      return "text";
    case "object":
      return "object";
    case "array":
      return "array";
    case "secret":
    case "password":
      return "secret";
    case "string":
      return "string";
    default:
      return "unsupported";
  }
}

function mapKind(row: KindApiRow): KindSchema {
  const fields = Object.entries(row.params ?? {}).map(([name, f]) => {
    const type = schemaType(f.type);
    return {
      name,
      title: f.label ?? name,
      type,
      default: f.default,
      required: f.required ?? (f.default === undefined),
      description: f.description,
      advanced: f.advanced,
      minimum: f.minimum,
      maximum: f.maximum,
      rawType: type === "unsupported" ? f.type : undefined,
    } as SchemaFieldDef;
  });
  return { kind: row.id, label: row.label, version: "1", fields };
}

function primaryEndpoint(values: Record<string, string | number | boolean>): string {
  const key = ["host", "broker_uri", "broker_url", "endpoint_url", "base_url", "serial_port"].find((n) => values[n] !== undefined && values[n] !== "");
  if (!key) return "-";
  if (key === "host" && values.port !== undefined) return `${values.host}:${values.port}`;
  return String(values[key]);
}

function mapTransport(row: TransportRow): Transport {
  const config = parseJsonObject(row.params_json);
  const enabled = row.enabled === "1";
  return {
    id: row.id,
    name: row.name,
    kind: row.kind,
    endpoint: primaryEndpoint(config),
    status: enabled ? "offline" : "disabled",
    enabled,
    pointCount: 0,
    updatedAt: row.updated_at ?? null,
    config,
    scheduler: parseJsonObject(row.scheduler_json),
  };
}

function toApiBody(payload: TransportPayload) {
  return {
    id: payload.id,
    name: payload.name,
    kind: payload.kind,
    enabled: payload.enabled ? 1 : 0,
    params_json: payload.params_json,
    scheduler_json: payload.scheduler_json ?? {},
  };
}

export async function fetchKinds(): Promise<KindSchema[]> {
  const rows = await apiGet<KindApiRow[]>("/transports/kinds");
  cachedKinds = rows.map(mapKind);
  return cachedKinds;
}

export function getKindSchema(kind: string): KindSchema | undefined {
  return cachedKinds.find((k) => k.kind === kind);
}

export function kindLabel(kind: string): string {
  return getKindSchema(kind)?.label ?? kind;
}

export async function fetchTransports(): Promise<Transport[]> {
  const [rows, runtime, datapoints] = await Promise.all([
    apiGet<TransportRow[]>("/transports"),
    apiGet<RuntimeTransportRow[]>("/runtime/transports").catch(() => []),
    apiGet<DatapointRow[]>("/datapoints").catch(() => []),
  ]);
  const runtimeById = new Map(runtime.map((item) => [item.id, item.state]));
  const pointCounts = new Map<string, number>();
  datapoints.forEach((point) => {
    pointCounts.set(
      point.transport_id,
      (pointCounts.get(point.transport_id) ?? 0) + 1,
    );
  });
  return rows.map((row) => {
    const transport = mapTransport(row);
    const state = runtimeById.get(row.id);
    transport.pointCount = pointCounts.get(row.id) ?? 0;
    if (!transport.enabled) transport.status = "disabled";
    else if (state === "connected") transport.status = "online";
    else if (state === "connecting") transport.status = "reconnecting";
    else if (state === "error") transport.status = "error";
    else transport.status = "offline";
    return transport;
  });
}

export async function createTransport(payload: TransportPayload): Promise<void> {
  await apiPost("/transports", toApiBody(payload));
}

export async function updateTransport(id: string, payload: TransportPayload): Promise<void> {
  await apiPut(`/transports/${encodeURIComponent(id)}`, toApiBody(payload));
}

export async function deleteTransport(id: string): Promise<void> {
  await apiDelete(`/transports/${encodeURIComponent(id)}`);
}

export interface TestResult {
  ok: boolean;
  latencyMs?: number;
  endpoint: string;
  message: string;
  at: string;
  errorType?: string;
  suggestions?: string[];
}

export async function testConnection(
  kind: string,
  values: Record<string, unknown>,
  id?: string,
): Promise<TestResult> {
  const endpoint = primaryEndpoint(values as Record<string, string | number | boolean>);
  const result = await apiPost<Partial<TestResult>>(
    `/transports/${encodeURIComponent(id ?? kind)}/test`,
    { kind, params_json: values },
  );
  return {
    ok: !!result.ok,
    latencyMs: result.latencyMs,
    endpoint: result.endpoint ?? endpoint,
    message: result.message ?? (result.ok ? "连接成功" : "连接失败"),
    at: result.at ?? new Date().toLocaleTimeString("zh-CN", { hour12: false }),
    errorType: result.errorType,
    suggestions: result.suggestions,
  };
}
