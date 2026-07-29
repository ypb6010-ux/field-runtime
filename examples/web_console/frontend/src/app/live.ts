import { apiGet } from "./api";

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
  ageSeconds: number;
  tags: string[];
  justUpdated?: boolean;
}

export interface TrendPoint {
  t: string;
  ts: number;
  value: number;
}

export const STALE_THRESHOLD_S = 30;

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

interface LatestPoint {
  id: string;
  value: number | boolean | string | null;
  quality: string;
  ts: number;
}

function quality(value: string): Quality {
  if (value === "Ok" || value === "Good") return "Good";
  if (value === "Error" || value === "Bad") return "Bad";
  return "Uncertain";
}

/** Refresh the runtime's latest cached point snapshot via HTTP. */
export async function readDatapoint(id: string): Promise<ReadResult> {
  const started = performance.now();
  try {
    const point = await apiGet<LatestPoint>(
      `/data/points/${encodeURIComponent(id)}`,
    );
    if (point.value === null) {
      return {
        ok: false,
        errorType: "Missing",
        message: "运行时尚无该点位的有效值",
      };
    }
    return {
      ok: true,
      value: point.value,
      quality: quality(point.quality),
      latencyMs: Math.max(0, Math.round(performance.now() - started)),
      timestamp: new Date(point.ts || Date.now()).toLocaleTimeString(
        "zh-CN",
        { hour12: false },
      ),
    };
  } catch (error) {
    return {
      ok: false,
      errorType: "HTTP",
      message: error instanceof Error ? error.message : "读取运行时快照失败",
      suggestions: ["确认运行时正在运行", "确认点位仍存在于活动配置"],
    };
  }
}
