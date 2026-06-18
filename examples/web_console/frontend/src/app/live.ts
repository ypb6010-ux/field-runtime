/**
 * Live 实时监控数据层。
 * 生产环境中：
 *   WS  dp/*          → useWsStream hook，latest-wins，支持订阅 / 自动重连
 *   GET /data/latest  → 初始化快照
 *   POST /data/read   → 立即拉取单点
 */

export type DataType = "number" | "boolean" | "string";
export type Quality = "Good" | "Bad" | "Uncertain";
export type DpStatus = "normal" | "alarm" | "error" | "offline" | "stale";
export type Trend = "up" | "down" | "flat";
export type WsState = "connected" | "reconnecting" | "disconnected";

export interface Datapoint {
  id: string;
  name: string;
  address: string;
  transportId: string;
  transportName: string;
  dataType: DataType;
  unit: string;
  quality: Quality;
  status: DpStatus;
  value: number | boolean | string;
  prevValue?: number | boolean | string;
  trend: Trend;
  timestamp: string;
  /** 距上次更新的秒数（> staleThreshold 视为 stale） */
  ageSeconds: number;
  tags: string[];
  /** 标记本次 tick 值是否刚刚变化（用于高亮动画） */
  justUpdated?: boolean;
}

export interface TrendPoint {
  t: string; // HH:MM:SS
  value: number;
}

export const STALE_THRESHOLD_S = 30;

// ---- Transports (供 Filter 下拉) ----
export const LIVE_TRANSPORTS = [
  { id: "t1", name: "车间 A · PLC-01" },
  { id: "t2", name: "车间 A · PLC-02" },
  { id: "t4", name: "锅炉房 · 温控" },
  { id: "t3", name: "能源站 · 电表网关" },
];

// ---- Seed datapoints ----
export const SEED_DATAPOINTS: Datapoint[] = [
  { id: "dp1", name: "主轴转速", address: "4x0001", transportId: "t1", transportName: "车间 A · PLC-01", dataType: "number", unit: "rpm", quality: "Good", status: "normal", value: 1480, prevValue: 1460, trend: "up", timestamp: "10:42:18", ageSeconds: 2, tags: ["主轴", "运动"] },
  { id: "dp2", name: "主轴温度", address: "4x0002", transportId: "t1", transportName: "车间 A · PLC-01", dataType: "number", unit: "°C", quality: "Good", status: "alarm", value: 88.4, prevValue: 85.1, trend: "up", timestamp: "10:42:17", ageSeconds: 3, tags: ["温度", "主轴"] },
  { id: "dp3", name: "液压压力", address: "4x0010", transportId: "t1", transportName: "车间 A · PLC-01", dataType: "number", unit: "bar", quality: "Uncertain", status: "normal", value: 142.8, prevValue: 143.0, trend: "down", timestamp: "10:42:15", ageSeconds: 5, tags: ["液压"] },
  { id: "dp4", name: "急停按钮", address: "0x0001", transportId: "t1", transportName: "车间 A · PLC-01", dataType: "boolean", unit: "", quality: "Good", status: "normal", value: false, trend: "flat", timestamp: "10:42:00", ageSeconds: 18, tags: ["安全"] },
  { id: "dp5", name: "错误代码", address: "4x0100", transportId: "t1", transportName: "车间 A · PLC-01", dataType: "string", unit: "", quality: "Bad", status: "error", value: "E_OVERHEAT", trend: "flat", timestamp: "10:41:55", ageSeconds: 23, tags: ["故障"] },
  { id: "dp6", name: "锅炉水温", address: "4x0001", transportId: "t4", transportName: "锅炉房 · 温控", dataType: "number", unit: "°C", quality: "Good", status: "normal", value: 76.2, prevValue: 75.8, trend: "up", timestamp: "10:42:16", ageSeconds: 4, tags: ["温度", "锅炉"] },
  { id: "dp7", name: "燃气流量", address: "4x0002", transportId: "t4", transportName: "锅炉房 · 温控", dataType: "number", unit: "m³/h", quality: "Good", status: "normal", value: 23.5, prevValue: 23.5, trend: "flat", timestamp: "10:42:10", ageSeconds: 8, tags: ["流量", "锅炉"] },
  { id: "dp8", name: "电网功率", address: "4x0001", transportId: "t3", transportName: "能源站 · 电表网关", dataType: "number", unit: "kW", quality: "Uncertain", status: "stale", value: 148.3, trend: "flat", timestamp: "10:41:22", ageSeconds: 56, tags: ["电力", "能源"] },
  { id: "dp9", name: "总电量", address: "4x0002", transportId: "t3", transportName: "能源站 · 电表网关", dataType: "number", unit: "kWh", quality: "Bad", status: "offline", value: 0, trend: "flat", timestamp: "10:40:11", ageSeconds: 127, tags: ["电力"] },
  { id: "dp10", name: "进给速度", address: "4x0003", transportId: "t2", transportName: "车间 A · PLC-02", dataType: "number", unit: "mm/min", quality: "Good", status: "normal", value: 320, prevValue: 300, trend: "up", timestamp: "10:42:18", ageSeconds: 1, tags: ["运动"] },
];

// ---- 生成趋势历史 ----
export function makeTrendHistory(base: number, points = 90): TrendPoint[] {
  const now = new Date(2026, 5, 18, 10, 42, 18);
  return Array.from({ length: points }, (_, i) => {
    const t = new Date(now.getTime() - (points - 1 - i) * 10_000);
    const hms = t.toLocaleTimeString("zh-CN", { hour12: false });
    const noise = (Math.random() - 0.5) * base * 0.06;
    const drift = Math.sin((i / points) * Math.PI) * base * 0.08;
    return { t: hms, value: parseFloat((base + drift + noise).toFixed(2)) };
  });
}

const delay = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

// ---- Mock /data/read ----
export interface ReadResult {
  ok: boolean;
  value?: number | boolean | string;
  quality?: Quality;
  latencyMs?: number;
  timestamp?: string;
  errorType?: string;
  message?: string;
  suggestions?: string[];
}

export async function readDatapoint(id: string): Promise<ReadResult> {
  await delay(800 + Math.random() * 600);
  const dp = SEED_DATAPOINTS.find((d) => d.id === id);
  if (!dp || dp.status === "offline" || id === "dp9") {
    return {
      ok: false,
      errorType: "Timeout",
      message: "设备无响应 timeout 3000ms",
      suggestions: ["transport 状态是否正常", "点位地址是否正确", "网络连通性", "防火墙是否放行"],
    };
  }
  return {
    ok: true,
    value: dp.value,
    quality: dp.quality,
    latencyMs: Math.round(15 + Math.random() * 50),
    timestamp: new Date().toLocaleTimeString("zh-CN", { hour12: false }),
  };
}

// ---- Simulate WS tick：随机更新几个 datapoints ----
export function simWsTick(prev: Datapoint[]): Datapoint[] {
  return prev.map((dp) => {
    if (dp.status === "offline" || dp.status === "stale") return { ...dp, justUpdated: false };
    if (Math.random() > 0.35) return { ...dp, justUpdated: false };

    const delta = dp.dataType === "number"
      ? (Math.random() - 0.48) * (Number(dp.value) * 0.02 + 0.5)
      : 0;
    const newVal = dp.dataType === "number"
      ? parseFloat((Number(dp.value) + delta).toFixed(2))
      : dp.dataType === "boolean"
        ? Math.random() > 0.97 ? !dp.value : dp.value
        : dp.value;

    const trend: Trend =
      dp.dataType === "number"
        ? delta > 0.01 ? "up" : delta < -0.01 ? "down" : "flat"
        : "flat";

    return {
      ...dp,
      prevValue: dp.value,
      value: newVal,
      trend,
      timestamp: new Date().toLocaleTimeString("zh-CN", { hour12: false }),
      ageSeconds: 1,
      quality: Math.random() > 0.98 ? "Uncertain" : dp.quality === "Bad" ? "Bad" : "Good",
      justUpdated: true,
    };
  });
}
