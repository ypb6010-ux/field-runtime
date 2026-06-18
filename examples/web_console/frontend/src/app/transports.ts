/**
 * Transport（协议连接）数据与模拟接口。
 *
 * 关键设计：协议配置表单不是前端写死的，而是由后端
 *   GET /transports/kinds  返回的 JSON Schema 动态渲染。
 * 后端新增一种 kind，前端无需改代码即可出现新协议类型与对应表单。
 */

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
  | "text" // 多行文本 / 证书
  | "object" // 高级：对象（折叠区）
  | "array" // 高级：数组（折叠区）
  | "unsupported"; // 前端暂不支持，走 fallback

export interface SchemaFieldDef {
  name: string;
  title: string;
  type: SchemaFieldType;
  required?: boolean;
  default?: string | number | boolean;
  description?: string;
  placeholder?: string;
  unit?: string;
  options?: { label: string; value: string }[];
  advanced?: boolean; // 归入“高级配置”折叠区
  rawType?: string; // unsupported 时展示原始类型
}

export interface KindSchema {
  kind: string;
  label: string;
  version: string;
  fields: SchemaFieldDef[];
}

// ---- 各 kind 的 Schema（模拟后端 /transports/kinds 返回）----
const MODBUS_TCP: KindSchema = {
  kind: "modbus_tcp",
  label: "Modbus TCP",
  version: "1.2.0",
  fields: [
    { name: "host", title: "Host", type: "string", required: true, placeholder: "10.20.1.11", description: "PLC / 网关 IP 地址或主机名" },
    { name: "port", title: "Port", type: "number", required: true, default: 502, description: "Modbus TCP 端口" },
    { name: "unit_id", title: "Unit ID", type: "number", required: true, default: 1, description: "从站地址 (1-247)" },
    { name: "timeout", title: "Timeout", type: "number", default: 3000, unit: "ms", description: "请求超时" },
    { name: "retry", title: "Retry", type: "number", default: 3, description: "失败重试次数" },
  ],
};

const MODBUS_RTU: KindSchema = {
  kind: "modbus_rtu",
  label: "Modbus RTU",
  version: "1.1.0",
  fields: [
    { name: "serial_port", title: "Serial Port", type: "string", required: true, placeholder: "/dev/ttyS0", description: "串口设备路径" },
    { name: "baud_rate", title: "Baud Rate", type: "enum", required: true, default: "9600", options: [
      { label: "9600", value: "9600" }, { label: "19200", value: "19200" }, { label: "38400", value: "38400" }, { label: "115200", value: "115200" },
    ] },
    { name: "unit_id", title: "Unit ID", type: "number", required: true, default: 1 },
    { name: "parity", title: "Parity", type: "enum", default: "none", options: [
      { label: "None", value: "none" }, { label: "Even", value: "even" }, { label: "Odd", value: "odd" },
    ] },
    { name: "timeout", title: "Timeout", type: "number", default: 3000, unit: "ms" },
  ],
};

const MQTT: KindSchema = {
  kind: "mqtt",
  label: "MQTT",
  version: "2.0.1",
  fields: [
    { name: "broker_url", title: "Broker URL", type: "string", required: true, placeholder: "mqtt://10.20.3.8:1883", description: "MQTT Broker 地址" },
    { name: "client_id", title: "Client ID", type: "string", required: true, placeholder: "idc-gateway-01", description: "客户端唯一标识" },
    { name: "username", title: "Username", type: "string" },
    { name: "password", title: "Password", type: "secret", description: "认证密码，保存后脱敏" },
    { name: "qos", title: "QoS", type: "enum", default: "1", options: [
      { label: "0 - 至多一次", value: "0" }, { label: "1 - 至少一次", value: "1" }, { label: "2 - 恰好一次", value: "2" },
    ] },
    { name: "topic_prefix", title: "Topic Prefix", type: "string", placeholder: "factory/line-a", description: "订阅 / 发布主题前缀", advanced: true },
    { name: "keep_alive", title: "Keep Alive", type: "number", default: 60, unit: "s", advanced: true },
    { name: "clean_session", title: "Clean Session", type: "boolean", default: true, description: "断开后是否清除会话状态", advanced: true },
  ],
};

const OPC_UA: KindSchema = {
  kind: "opc_ua",
  label: "OPC UA",
  version: "1.4.0",
  fields: [
    { name: "endpoint_url", title: "Endpoint URL", type: "string", required: true, placeholder: "opc.tcp://10.20.2.5:4840", description: "OPC UA 服务端点" },
    { name: "security_mode", title: "Security Mode", type: "enum", required: true, default: "None", options: [
      { label: "None", value: "None" }, { label: "Sign", value: "Sign" }, { label: "SignAndEncrypt", value: "SignAndEncrypt" },
    ] },
    { name: "security_policy", title: "Security Policy", type: "enum", default: "None", options: [
      { label: "None", value: "None" }, { label: "Basic256Sha256", value: "Basic256Sha256" }, { label: "Aes256Sha256RsaPss", value: "Aes256Sha256RsaPss" },
    ] },
    { name: "username", title: "Username", type: "string" },
    { name: "password", title: "Password", type: "secret" },
    { name: "certificate", title: "Certificate", type: "text", description: "PEM 证书内容，或上传证书文件", placeholder: "-----BEGIN CERTIFICATE-----", advanced: true },
    { name: "trust_server_cert", title: "Trust Server Certificate", type: "boolean", default: false, description: "信任服务端证书（跳过校验）", advanced: true },
    { name: "session_timeout", title: "Session Timeout", type: "number", default: 60000, unit: "ms", description: "会话超时时间", advanced: true },
  ],
};

// HTTP：故意包含一个前端暂不支持的字段，用于演示 Schema 不兼容 fallback
const HTTP: KindSchema = {
  kind: "http",
  label: "HTTP",
  version: "0.9.0",
  fields: [
    { name: "base_url", title: "Base URL", type: "string", required: true, placeholder: "https://api.example.com" },
    { name: "method", title: "Method", type: "enum", default: "GET", options: [
      { label: "GET", value: "GET" }, { label: "POST", value: "POST" },
    ] },
    { name: "token", title: "Bearer Token", type: "secret" },
    { name: "poll_interval", title: "Poll Interval", type: "number", default: 5000, unit: "ms" },
    { name: "headers", title: "Headers", type: "object", description: "自定义请求头（高级）", advanced: true },
    { name: "retry_matrix", title: "Retry Matrix", type: "unsupported", rawType: "matrix<int,int>", description: "重试退避矩阵" },
  ],
};

const KINDS: KindSchema[] = [MODBUS_TCP, MODBUS_RTU, MQTT, OPC_UA, HTTP];

// ---- 协议连接记录 ----
export interface Transport {
  id: string;
  name: string;
  kind: string;
  endpoint: string;
  status: TransportStatus;
  enabled: boolean;
  lastConnectedAt: string | null;
  lastError: string | null;
  pointCount: number;
  updatedBy: string;
  tags: string[];
  config: Record<string, string | number | boolean>;
}

export const SEED_TRANSPORTS: Transport[] = [
  { id: "t1", name: "车间 A · PLC-01", kind: "modbus_tcp", endpoint: "10.20.1.11:502", status: "online", enabled: true, lastConnectedAt: "2026-06-18 10:42:01", lastError: null, pointCount: 128, updatedBy: "张工", tags: ["车间A", "PLC"], config: { host: "10.20.1.11", port: 502, unit_id: 1, timeout: 3000, retry: 3 } },
  { id: "t2", name: "车间 A · PLC-02", kind: "modbus_tcp", endpoint: "10.20.1.12:502", status: "online", enabled: true, lastConnectedAt: "2026-06-18 10:41:55", lastError: null, pointCount: 96, updatedBy: "张工", tags: ["车间A", "PLC"], config: { host: "10.20.1.12", port: 502, unit_id: 2, timeout: 3000, retry: 3 } },
  { id: "t3", name: "能源站 · 电表网关", kind: "opc_ua", endpoint: "opc.tcp://10.20.2.5:4840", status: "reconnecting", enabled: true, lastConnectedAt: "2026-06-18 10:39:12", lastError: "BadConnectionRejected: 会话超时", pointCount: 64, updatedBy: "王操作", tags: ["能源"], config: { endpoint_url: "opc.tcp://10.20.2.5:4840", security_mode: "Sign", security_policy: "Basic256Sha256", username: "energy" } },
  { id: "t4", name: "锅炉房 · 温控", kind: "mqtt", endpoint: "mqtt://10.20.3.8:1883", status: "online", enabled: true, lastConnectedAt: "2026-06-18 10:42:09", lastError: null, pointCount: 42, updatedBy: "王操作", tags: ["锅炉"], config: { broker_url: "mqtt://10.20.3.8:1883", client_id: "boiler-01", qos: "1" } },
  { id: "t5", name: "仓储 · RFID 读头", kind: "modbus_rtu", endpoint: "/dev/ttyS0", status: "error", enabled: true, lastConnectedAt: "2026-06-18 09:58:30", lastError: "SerialException: 设备无响应 (timeout 3000ms)", pointCount: 0, updatedBy: "张工", tags: ["仓储"], config: { serial_port: "/dev/ttyS0", baud_rate: "9600", unit_id: 5 } },
  { id: "t6", name: "产线 B · 视觉检测", kind: "opc_ua", endpoint: "opc.tcp://10.20.4.2:4840", status: "disabled", enabled: false, lastConnectedAt: "2026-06-15 18:20:00", lastError: null, pointCount: 0, updatedBy: "张工", tags: ["产线B", "视觉"], config: { endpoint_url: "opc.tcp://10.20.4.2:4840", security_mode: "None" } },
  { id: "t7", name: "三方平台 · HTTP 拉取", kind: "http", endpoint: "https://api.partner.com", status: "offline", enabled: true, lastConnectedAt: "2026-06-18 08:10:44", lastError: null, pointCount: 12, updatedBy: "王操作", tags: ["三方"], config: { base_url: "https://api.partner.com", method: "GET", poll_interval: 5000 } },
];

export const ALL_TAGS = ["车间A", "PLC", "能源", "锅炉", "仓储", "产线B", "视觉", "三方"];

// ---- 模拟接口 ----
const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

// 用于演示 /transports/kinds 加载失败
let kindsFailMode = false;
export function setKindsFailMode(v: boolean) {
  kindsFailMode = v;
}

/** GET /transports/kinds */
export async function fetchKinds(): Promise<KindSchema[]> {
  await delay(650);
  if (kindsFailMode) {
    throw new Error("GET /transports/kinds 失败：502 Bad Gateway");
  }
  return KINDS;
}

export function getKindSchema(kind: string): KindSchema | undefined {
  return KINDS.find((k) => k.kind === kind);
}

export function kindLabel(kind: string): string {
  return getKindSchema(kind)?.label ?? kind;
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

/** 主端点字段（用于 test 结果展示与失败模拟） */
function primaryEndpoint(kind: string, values: Record<string, unknown>): string {
  const f = ["host", "broker_url", "endpoint_url", "base_url", "serial_port"].find((n) => values[n]);
  return f ? String(values[f]) : "—";
}

/** POST /transports/test —— endpoint 含 "fail" 可模拟失败 */
export async function testConnection(
  kind: string,
  values: Record<string, unknown>,
): Promise<TestResult> {
  await delay(1300);
  const endpoint = primaryEndpoint(kind, values);
  const at = new Date().toLocaleTimeString("zh-CN", { hour12: false });

  if (/fail|down|unreach|0\.0\.0\.0/i.test(endpoint)) {
    return {
      ok: false,
      endpoint,
      at,
      message: "连接被拒绝：无法在超时时间内建立连接。",
      errorType: "ECONNREFUSED",
      suggestions: ["Host / Broker URL 是否正确", "Port 是否开放", "认证信息是否正确", "网络是否可达", "防火墙是否放行"],
    };
  }
  return {
    ok: true,
    latencyMs: Math.round(8 + Math.random() * 40),
    endpoint,
    at,
    message: "连接成功，握手与读取测试通过。",
  };
}
