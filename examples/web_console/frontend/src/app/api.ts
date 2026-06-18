/**
 * 后端 API 客户端。统一响应包 { code, message, data }；鉴权 Bearer token（localStorage）。
 * dev 下 /api 与 /ws 由 vite proxy 转发到后端 :8080（见 vite.config.ts）。
 */

const TOKEN_KEY = "wc_token";

export function getToken(): string {
  return localStorage.getItem(TOKEN_KEY) ?? "";
}
export function setToken(t: string): void {
  localStorage.setItem(TOKEN_KEY, t);
}
export function clearToken(): void {
  localStorage.removeItem(TOKEN_KEY);
}

export class ApiError extends Error {
  code: number;
  status: number;
  constructor(code: number, message: string, status = 0) {
    super(message);
    this.code = code;
    this.status = status;
  }
}

interface Envelope<T> {
  code: number;
  message: string;
  data: T;
}

export async function api<T = unknown>(path: string, opts: RequestInit = {}): Promise<T> {
  const headers: Record<string, string> = { ...(opts.headers as Record<string, string>) };
  if (opts.body && !headers["Content-Type"]) headers["Content-Type"] = "application/json";
  const tok = getToken();
  if (tok) headers["Authorization"] = `Bearer ${tok}`;

  let resp: Response;
  try {
    resp = await fetch(`/api/v1${path}`, { ...opts, headers });
  } catch {
    throw new ApiError(-1, "无法连接服务器，请检查后端服务。");
  }

  let env: Envelope<T>;
  try {
    env = (await resp.json()) as Envelope<T>;
  } catch {
    throw new ApiError(-1, `HTTP ${resp.status}`, resp.status);
  }
  if (env.code !== 0) throw new ApiError(env.code, env.message || `error ${env.code}`, resp.status);
  return env.data;
}

export const apiGet = <T = unknown>(p: string) => api<T>(p);
export const apiPost = <T = unknown>(p: string, body?: unknown) =>
  api<T>(p, { method: "POST", body: body !== undefined ? JSON.stringify(body) : undefined });
export const apiPut = <T = unknown>(p: string, body?: unknown) =>
  api<T>(p, { method: "PUT", body: JSON.stringify(body ?? {}) });
export const apiDelete = <T = unknown>(p: string) => api<T>(p, { method: "DELETE" });

// ---- WebSocket 实时流 ----
export interface WsSnapshot {
  type: string;
  datapoints?: { id: string; value: unknown; quality: string; ts: number }[];
  transports?: { id: string; kind: string; state: string }[];
}

/** 打开 /ws/stream 并默认订阅 dp/* + transport/*。返回 WebSocket（调用方负责 close）。 */
export function openStream(
  onSnapshot: (s: WsSnapshot) => void,
  topics: string[] = ["dp/*", "transport/*"],
): WebSocket {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const tok = getToken();
  const ws = new WebSocket(`${proto}://${location.host}/ws/stream${tok ? `?token=${encodeURIComponent(tok)}` : ""}`);
  ws.onopen = () => ws.send(JSON.stringify({ op: "subscribe", topics }));
  ws.onmessage = (e) => {
    try {
      const msg = JSON.parse(e.data) as WsSnapshot;
      if (msg.type === "snapshot") onSnapshot(msg);
    } catch {
      /* ignore */
    }
  };
  return ws;
}
