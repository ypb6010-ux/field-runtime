import { Cpu } from "lucide-react";
import type { PageKey, Role } from "../types";
import { visibleGroups } from "../nav";
import { cn } from "./ui/utils";
import { Tooltip, TooltipContent, TooltipTrigger } from "./ui/tooltip";

interface SidebarProps {
  role: Role;
  current: PageKey;
  collapsed: boolean;
  draftScopedKeys?: PageKey[];
  onNavigate: (page: PageKey) => void;
}

export function Sidebar({ role, current, collapsed, draftScopedKeys = [], onNavigate }: SidebarProps) {
  const groups = visibleGroups(role);

  return (
    <aside
      className={cn(
        "flex h-full shrink-0 flex-col bg-sidebar text-sidebar-foreground transition-[width] duration-200",
        collapsed ? "w-16" : "w-60",
      )}
    >
      {/* 品牌区 */}
      <div className="flex h-14 items-center gap-2.5 border-b border-sidebar-border px-4">
        <div className="flex size-8 shrink-0 items-center justify-center rounded-md bg-primary">
          <Cpu className="size-5 text-primary-foreground" />
        </div>
        {!collapsed && (
          <div className="flex flex-col leading-tight">
            <span className="text-sm text-white">IDC Gateway</span>
            <span className="text-[11px] text-sidebar-foreground/60">协议转换控制台</span>
          </div>
        )}
      </div>

      {/* 导航分组 */}
      <nav className="flex-1 overflow-y-auto px-2 py-3">
        {groups.map((group) => (
          <div key={group.id} className="mb-4">
            {!collapsed && (
              <div className="mb-1 px-2 text-[11px] uppercase tracking-wide text-sidebar-foreground/40">
                {group.title}
              </div>
            )}
            <ul className="space-y-0.5">
              {group.items.map((item) => {
                const active = item.key === current;
                const Icon = item.icon;
                const hasDraft = draftScopedKeys.includes(item.key);
                const btn = (
                  <button
                    type="button"
                    onClick={() => onNavigate(item.key)}
                    className={cn(
                      "group flex w-full items-center gap-2.5 rounded-md px-2.5 py-2 text-sm transition-colors",
                      collapsed && "justify-center px-0",
                      active
                        ? "bg-sidebar-primary text-sidebar-primary-foreground"
                        : "text-sidebar-foreground hover:bg-sidebar-accent hover:text-sidebar-accent-foreground",
                    )}
                  >
                    <span className="relative">
                      <Icon className="size-4 shrink-0" />
                      {hasDraft && (
                        <span className="absolute -right-1 -top-1 size-1.5 rounded-full bg-status-draft" />
                      )}
                    </span>
                    {!collapsed && (
                      <span className="flex flex-1 items-center justify-between">
                        <span>{item.label}</span>
                        <span className="text-[11px] text-current/50">{item.en}</span>
                      </span>
                    )}
                  </button>
                );

                return (
                  <li key={item.key}>
                    {collapsed ? (
                      <Tooltip>
                        <TooltipTrigger asChild>{btn}</TooltipTrigger>
                        <TooltipContent side="right">
                          {item.label} · {item.en}
                        </TooltipContent>
                      </Tooltip>
                    ) : (
                      btn
                    )}
                  </li>
                );
              })}
            </ul>
          </div>
        ))}
      </nav>

      {/* 底部版本信息 */}
      {!collapsed && (
        <div className="border-t border-sidebar-border px-4 py-3 text-[11px] text-sidebar-foreground/40">
          v0.1.0 · 骨架版本
        </div>
      )}
    </aside>
  );
}
