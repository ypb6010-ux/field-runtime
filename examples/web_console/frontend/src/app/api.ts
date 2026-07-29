/**
 * 后端 API 客户端。统一响应包 { code, message, data }；鉴权 Bearer token（sessionStorage）。
 * dev 下 /api 与 /ws 由 vite proxy 转发到后端 :8080（见 vite.config.ts）。
 */

const TOKEN_KEY = "wc_token";

export function isValidResourceId(value: string): boolean {
  return value.length >= 1
    && value.length <= 128
    && !/[\s/\\?#%\u0000-\u001f\u007f]/.test(value);
}

export function getToken(): string {
  return sessionStorage.getItem(TOKEN_KEY) ?? "";
}
export function setToken(t: string): void {
  sessionStorage.setItem(TOKEN_KEY, t);
}
export function clearToken(): void {
  sessionStorage.removeItem(TOKEN_KEY);
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
  const controller = opts.signal ? null : new AbortController();
  const timeout = controller
    ? window.setTimeout(() => controller.abort(), 15_000)
    : undefined;
  try {
    resp = await fetch(`/api/v1${path}`, {
      ...opts,
      headers,
      signal: opts.signal ?? controller?.signal,
    });
  } catch {
    throw new ApiError(-1, "无法连接服务器，请检查后端服务。");
  } finally {
    if (timeout !== undefined) window.clearTimeout(timeout);
  }

  let env: Envelope<T>;
  try {
    env = (await resp.json()) as Envelope<T>;
  } catch {
    throw new ApiError(-1, `HTTP ${resp.status}`, resp.status);
  }
  if (env.code !== 0) {
    if (resp.status === 401 || env.code === 2001) {
      clearToken();
      window.dispatchEvent(new Event("field-console:unauthorized"));
    }
    throw new ApiError(env.code, env.message || `error ${env.code}`, resp.status);
  }
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

export type StreamState = "connecting" | "connected" | "reconnecting" | "disconnected";

export interface StreamConnection {
  close: () => void;
  reconnect: () => void;
}

/** 自动重连的实时流。重试采用 1s..30s 指数退避，并在连接期间发送心跳。 */
export function connectStream({
  onSnapshot,
  onState,
  topics = ["dp/*", "transport/*"],
}: {
  onSnapshot: (snapshot: WsSnapshot) => void;
  onState?: (state: StreamState) => void;
  topics?: string[];
}): StreamConnection {
  let socket: WebSocket | null = null;
  let stopped = false;
  let retryCount = 0;
  let retryTimer: number | undefined;
  let heartbeatTimer: number | undefined;

  const clearTimers = () => {
    if (retryTimer !== undefined) window.clearTimeout(retryTimer);
    if (heartbeatTimer !== undefined) window.clearInterval(heartbeatTimer);
    retryTimer = undefined;
    heartbeatTimer = undefined;
  };

  const scheduleReconnect = () => {
    if (stopped || retryTimer !== undefined) return;
    onState?.("reconnecting");
    const delay = Math.min(30_000, 1_000 * 2 ** Math.min(retryCount++, 5));
    retryTimer = window.setTimeout(() => {
      retryTimer = undefined;
      open();
    }, delay);
  };

  const open = () => {
    if (stopped) return;
    clearTimers();
    onState?.(retryCount === 0 ? "connecting" : "reconnecting");
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const token = getToken();
    const ws = new WebSocket(
      `${proto}://${location.host}/ws/stream${token ? `?token=${encodeURIComponent(token)}` : ""}`,
    );
    socket = ws;
    ws.onopen = () => {
      if (socket !== ws || stopped) return;
      retryCount = 0;
      onState?.("connected");
      ws.send(JSON.stringify({ op: "subscribe", topics }));
      heartbeatTimer = window.setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ op: "ping" }));
        }
      }, 15_000);
    };
    ws.onmessage = (event) => {
      if (socket !== ws || stopped) return;
      try {
        const message = JSON.parse(String(event.data)) as WsSnapshot;
        if (message.type === "snapshot") onSnapshot(message);
      } catch {
        // Ignore malformed server frames; the next valid snapshot is authoritative.
      }
    };
    ws.onerror = () => ws.close();
    ws.onclose = () => {
      if (socket !== ws) return;
      if (heartbeatTimer !== undefined) window.clearInterval(heartbeatTimer);
      heartbeatTimer = undefined;
      socket = null;
      scheduleReconnect();
    };
  };

  open();
  return {
    close: () => {
      stopped = true;
      clearTimers();
      socket?.close();
      socket = null;
      onState?.("disconnected");
    },
    reconnect: () => {
      stopped = false;
      clearTimers();
      socket?.close();
      socket = null;
      retryCount = 0;
      open();
    },
  };
}
