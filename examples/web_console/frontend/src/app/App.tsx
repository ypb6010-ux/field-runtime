import { useEffect, useState } from "react";
import { toast } from "sonner";
import type { AuthUser, PageKey, StatusTone } from "./types";
import { canAccess, defaultPage } from "./nav";
import { currentUser, hasPermission } from "./auth";
import { apiGet, apiPost, clearToken, connectStream, getToken } from "./api";
import { AppShell } from "./components/AppShell";
import { Toaster } from "./components/ui/sonner";
import { Login } from "./pages/Login";
import { Dashboard } from "./pages/Dashboard";
import { Protocols } from "./pages/Protocols";
import { Live } from "./pages/Live";
import { ConfigApply } from "./pages/ConfigApply";
import { Datapoints } from "./pages/Datapoints";
import { History } from "./pages/History";
import { UsersRoles } from "./pages/UsersRoles";
import { ApiDocs } from "./pages/ApiDocs";
import { Polling } from "./pages/Polling";
import { Conversion } from "./pages/Conversion";
import { Logs } from "./pages/Logs";
import { Settings } from "./pages/Settings";
import { ControlCenter } from "./pages/ControlCenter";

/** 受 config 权限影响、且当前存在未生效草稿的页面（侧栏小红点） */
const DRAFT_SCOPED: PageKey[] = ["protocols", "datapoints", "polling", "control", "config"];

function renderPage(
  page: PageKey,
  ctx: {
    role: AuthUser["role"];
    draftCount: number;
    canWriteProtocol: boolean;
    onNavigate: (p: PageKey) => void;
    onDraftIncrement: () => void;
  },
) {
  switch (page) {
    case "dashboard":
      return <Dashboard role={ctx.role} draftCount={ctx.draftCount} onNavigate={ctx.onNavigate} />;
    case "protocols":
      return <Protocols canWrite={ctx.canWriteProtocol} onDraftIncrement={ctx.onDraftIncrement} />;
    case "live":
      return <Live role={ctx.role} />;
    case "config":
      return <ConfigApply onChanged={ctx.onDraftIncrement} />;
    case "datapoints":
      return <Datapoints canWrite={ctx.canWriteProtocol} onDraftIncrement={ctx.onDraftIncrement} />;
    case "history":
      return <History />;
    case "users":
      return <UsersRoles canWrite={ctx.role === "admin"} />;
    case "apidocs":
      return <ApiDocs />;
    case "polling":
      return <Polling canWrite={ctx.canWriteProtocol} onDraftIncrement={ctx.onDraftIncrement} />;
    case "conversion":
      return <Conversion canWrite={ctx.role !== "viewer"} />;
    case "control":
      return <ControlCenter canWrite={ctx.role !== "viewer"} onChanged={ctx.onDraftIncrement} />;
    case "logs":
      return <Logs />;
    case "settings":
      return <Settings canWrite={ctx.role === "admin"} />;
    default:
      return null;
  }
}

export default function App() {
  const [user, setUser] = useState<AuthUser | null>(null);
  const [restoring, setRestoring] = useState(true);
  const [page, setPage] = useState<PageKey>("dashboard");
  const [collapsed, setCollapsed] = useState(false);

  const [draftCount, setDraftCount] = useState(0);
  const [systemStatus, setSystemStatus] = useState<StatusTone>("info");
  const [wsStatus, setWsStatus] = useState<StatusTone>("info");

  async function refreshDraftStatus() {
    if (!user || !hasPermission(user, "config:read")) {
      setDraftCount(0);
      return;
    }
    try {
      const status = await apiGet<{ draftDirty: boolean }>("/config/status");
      setDraftCount(status.draftDirty ? 1 : 0);
    } catch {
      // The header status is advisory; the Config page shows the full error.
    }
  }

  // Restore an existing bearer session and react immediately to global 401s.
  useEffect(() => {
    let active = true;
    const unauthorized = () => {
      clearToken();
      setUser(null);
      setRestoring(false);
    };
    window.addEventListener("field-console:unauthorized", unauthorized);
    if (!getToken()) {
      setRestoring(false);
    } else {
      currentUser()
        .then((restored) => {
          if (active) setUser(restored);
        })
        .catch(() => {
          clearToken();
        })
        .finally(() => {
          if (active) setRestoring(false);
        });
    }
    return () => {
      active = false;
      window.removeEventListener("field-console:unauthorized", unauthorized);
    };
  }, []);

  // Live system and WebSocket status; no simulated state transitions.
  useEffect(() => {
    if (!user) return;
    let active = true;
    const checkHealth = async () => {
      try {
        const health = await apiGet<{ status: string }>("/system/health");
        if (!active) return;
        setSystemStatus(
          health.status === "ok"
            ? "success"
            : health.status === "degraded"
              ? "warning"
              : "error",
        );
      } catch {
        if (active) setSystemStatus("error");
      }
    };
    checkHealth();
    refreshDraftStatus();
    const healthTimer = window.setInterval(checkHealth, 10_000);
    const stream = connectStream({
      topics: ["transport/*"],
      onSnapshot: () => {},
      onState: (state) => {
        if (!active) return;
        setWsStatus(
          state === "connected"
            ? "success"
            : state === "disconnected"
              ? "error"
              : state === "reconnecting"
                ? "warning"
                : "info",
        );
      },
    });
    return () => {
      active = false;
      window.clearInterval(healthTimer);
      stream.close();
    };
    // Recreate monitors only when the authenticated identity changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [user?.name]);

  if (restoring) {
    return (
      <div className="flex min-h-screen items-center justify-center bg-background text-sm text-muted-foreground">
        正在恢复会话…
      </div>
    );
  }

  if (!user) {
    return (
      <>
        <Login
          onAuthenticated={(u) => {
            setUser(u);
            setPage(defaultPage(u.role)); // 落地页由角色策略决定
            setSystemStatus("info");
            setWsStatus("info");
          }}
        />
        <Toaster position="top-center" richColors />
      </>
    );
  }

  // 防御性：当前页无权限时回退到默认页
  const safePage = canAccess(user.role, page) ? page : defaultPage(user.role);
  const canSeeConfig = hasPermission(user, "config:read");
  const canWriteProtocol = hasPermission(user, "protocol:write");

  const systemLabel =
    systemStatus === "success"
      ? "在线"
      : systemStatus === "warning"
        ? "降级"
        : systemStatus === "error"
          ? "异常"
          : "检查中";
  const wsLabel =
    wsStatus === "success"
      ? "正常"
      : wsStatus === "warning"
        ? "重连中"
        : wsStatus === "error"
          ? "断开"
          : "检查中";

  return (
    <>
      <AppShell
        role={user.role}
        userName={user.name}
        current={safePage}
        collapsed={collapsed}
        systemStatus={systemStatus}
        systemLabel={systemLabel}
        wsStatus={wsStatus}
        wsLabel={wsLabel}
        wsPulse={wsStatus === "warning"}
        canSeeConfig={canSeeConfig}
        draftCount={draftCount}
        draftScopedKeys={canSeeConfig ? DRAFT_SCOPED : []}
        onToggleSidebar={() => setCollapsed((c) => !c)}
        onNavigate={setPage}
        onOpenProfile={() => toast.message(`${user.name} · ${user.role}`)}
        onLogout={async () => {
          try {
            await apiPost("/auth/logout");
          } catch {
            // Local logout must still succeed if the server is unavailable.
          } finally {
            clearToken();
            setUser(null);
            setPage("dashboard");
          }
        }}
      >
        {renderPage(safePage, {
          role: user.role,
          draftCount,
          canWriteProtocol,
          onNavigate: setPage,
          onDraftIncrement: refreshDraftStatus,
        })}
      </AppShell>
      <Toaster position="top-center" richColors />
    </>
  );
}
