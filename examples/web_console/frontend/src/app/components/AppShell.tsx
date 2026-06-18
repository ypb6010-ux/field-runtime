import type { ReactNode } from "react";
import type { PageKey, Role, StatusTone } from "../types";
import { Sidebar } from "./Sidebar";
import { Header } from "./Header";

interface AppShellProps {
  role: Role;
  userName: string;
  current: PageKey;
  collapsed: boolean;
  systemStatus: StatusTone;
  systemLabel: string;
  wsStatus: StatusTone;
  wsLabel: string;
  wsPulse?: boolean;
  canSeeConfig: boolean;
  draftCount: number;
  draftScopedKeys?: PageKey[];
  onToggleSidebar: () => void;
  onNavigate: (page: PageKey) => void;
  onOpenProfile: () => void;
  onLogout: () => void;
  children: ReactNode;
}

export function AppShell(props: AppShellProps) {
  const {
    role,
    userName,
    current,
    collapsed,
    draftScopedKeys,
    onNavigate,
    children,
    ...header
  } = props;

  return (
    <div className="flex h-full w-full overflow-hidden bg-background">
      <Sidebar
        role={role}
        current={current}
        collapsed={collapsed}
        draftScopedKeys={draftScopedKeys}
        onNavigate={onNavigate}
      />
      <div className="flex min-w-0 flex-1 flex-col">
        <Header
          role={role}
          userName={userName}
          collapsed={collapsed}
          onNavigate={onNavigate}
          {...header}
        />
        <main className="flex-1 overflow-y-auto">{children}</main>
      </div>
    </div>
  );
}
