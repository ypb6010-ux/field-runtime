import { useEffect, useState } from "react";
import { toast } from "sonner";
import type { AuthUser, PageKey, StatusTone } from "./types";
import { canAccess, defaultPage } from "./nav";
import { hasPermission } from "./auth";
import { clearToken } from "./api";
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
import { Placeholder } from "./pages/Placeholder";
import { RoleNavigation } from "./pages/RoleNavigation";
import { ButtonStates } from "./pages/ButtonStates";
import { GlobalStates } from "./pages/GlobalStates";
import { StatePlayground } from "./pages/StatePlayground";

/** 受 config 权限影响、且当前存在未生效草稿的页面（侧栏小红点） */
const DRAFT_SCOPED: PageKey[] = ["conversion", "config"];

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
      return <ConfigApply />;
    case "datapoints":
      return <Datapoints canWrite={ctx.role === "admin"} onDraftIncrement={ctx.onDraftIncrement} />;
    case "history":
      return <History />;
    case "users":
      return <UsersRoles canWrite={ctx.role === "admin"} />;
    case "apidocs":
      return <ApiDocs />;
    case "polling":
      return <Polling canWrite={ctx.role === "admin"} onDraftIncrement={ctx.onDraftIncrement} />;
    case "conversion":
      return <Conversion canWrite={ctx.role !== "viewer"} onDraftIncrement={ctx.onDraftIncrement} />;
    case "logs":
      return <Logs />;
    case "settings":
      return <Settings canWrite={ctx.role === "admin"} />;
    case "spec-roles":
      return <RoleNavigation />;
    case "spec-buttons":
      return <ButtonStates />;
    case "spec-states":
      return <GlobalStates />;
    case "spec-live-playground":
      return <StatePlayground />;
    default:
      return <Placeholder page={page} />;
  }
}

export default function App() {
  const [user, setUser] = useState<AuthUser | null>(null);
  const [page, setPage] = useState<PageKey>("dashboard");
  const [collapsed, setCollapsed] = useState(false);

  // 模拟运行态
  const [draftCount, setDraftCount] = useState(3);
  const [systemStatus, setSystemStatus] = useState<StatusTone>("info"); // 登录后先“检查中”
  const [wsStatus, setWsStatus] = useState<StatusTone>("info");

  // 登录后：系统由“检查中”过渡到“在线”，WS 偶发重连
  useEffect(() => {
    if (!user) return;
    const t = setTimeout(() => {
      setSystemStatus("success");
      setWsStatus("success");
    }, 1200);
    const id = setInterval(() => {
      setWsStatus((prev) => (prev === "success" ? "warning" : "success"));
    }, 6000);
    return () => {
      clearTimeout(t);
      clearInterval(id);
    };
  }, [user]);

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
    systemStatus === "success" ? "在线" : systemStatus === "error" ? "异常" : "检查中";
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
        onOpenProfile={() => toast.message(`${user.name} · 个人信息（演示）`)}
        onLogout={() => {
          clearToken();
          setUser(null);
          setPage("dashboard");
        }}
      >
        {renderPage(safePage, {
          role: user.role,
          draftCount,
          canWriteProtocol,
          onNavigate: setPage,
          onDraftIncrement: () => setDraftCount((c) => c + 1),
        })}
      </AppShell>
      <Toaster position="top-center" richColors />
    </>
  );
}
