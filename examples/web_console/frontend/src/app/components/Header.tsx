import {
  ChevronDown,
  LogOut,
  PanelLeftClose,
  PanelLeftOpen,
  UserRound,
  CheckCircle2,
} from "lucide-react";
import type { PageKey, Role, StatusTone } from "../types";
import { ROLE_LABELS } from "../types";
import { StatusLight } from "./StatusLight";
import { Button } from "./ui/button";
import { Badge } from "./ui/badge";
import { Separator } from "./ui/separator";
import { Avatar, AvatarFallback } from "./ui/avatar";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "./ui/dropdown-menu";
import { Tooltip, TooltipContent, TooltipTrigger } from "./ui/tooltip";

interface HeaderProps {
  role: Role;
  userName: string;
  systemStatus: StatusTone;
  systemLabel: string;
  wsStatus: StatusTone;
  wsLabel: string;
  wsPulse?: boolean;
  /** 是否具备 config 权限（由后端 permissions 决定，而非角色） */
  canSeeConfig: boolean;
  draftCount: number;
  collapsed: boolean;
  onToggleSidebar: () => void;
  onNavigate: (page: PageKey) => void;
  onOpenProfile: () => void;
  onLogout: () => void;
}

export function Header({
  role,
  userName,
  systemStatus,
  systemLabel,
  wsStatus,
  wsLabel,
  wsPulse,
  canSeeConfig,
  draftCount,
  collapsed,
  onToggleSidebar,
  onNavigate,
  onOpenProfile,
  onLogout,
}: HeaderProps) {
  const hasDrafts = draftCount > 0;
  const initials = userName.slice(0, 2).toUpperCase();

  return (
    <header className="flex h-14 shrink-0 items-center justify-between border-b border-border bg-card px-4">
      {/* 左侧：折叠按钮 */}
      <div className="flex items-center gap-3">
        <Button variant="ghost" size="icon" className="size-8" onClick={onToggleSidebar}>
          {collapsed ? <PanelLeftOpen className="size-4" /> : <PanelLeftClose className="size-4" />}
        </Button>
      </div>

      {/* 右侧：状态区 */}
      <div className="flex items-center gap-3">
        {/* 系统 + WebSocket 状态灯 */}
        <div className="flex items-center gap-3 rounded-md border border-border bg-muted/40 px-3 py-1.5">
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="flex items-center gap-1.5">
                <span className="text-xs text-muted-foreground">系统</span>
                <StatusLight tone={systemStatus} label={systemLabel} pulse={systemStatus === "info"} />
              </span>
            </TooltipTrigger>
            <TooltipContent>系统连接状态：在线 / 检查中 / 异常</TooltipContent>
          </Tooltip>
          <Separator orientation="vertical" className="h-4" />
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="flex items-center gap-1.5">
                <span className="text-xs text-muted-foreground">WS</span>
                <StatusLight tone={wsStatus} label={wsLabel} pulse={wsPulse} />
              </span>
            </TooltipTrigger>
            <TooltipContent>WebSocket 通道：正常 / 重连中 / 断开</TooltipContent>
          </Tooltip>
        </div>

        {/* 未生效项 / 已同步（仅有 config 权限可见） */}
        {canSeeConfig &&
          (hasDrafts ? (
            <Button
              variant="outline"
              size="sm"
              className="h-8 gap-1.5 border-status-draft-border bg-status-draft-bg text-status-draft hover:bg-status-draft-bg/70 hover:text-status-draft"
              onClick={() => onNavigate("config")}
            >
              未生效项
              <Badge className="h-5 min-w-5 border-transparent bg-status-draft px-1.5 text-white">
                {draftCount}
              </Badge>
            </Button>
          ) : (
            <Tooltip>
              <TooltipTrigger asChild>
                <span className="flex items-center gap-1.5 rounded-md border border-status-success-border bg-status-success-bg px-2.5 py-1.5 text-xs text-status-success">
                  <CheckCircle2 className="size-3.5" />
                  已同步
                </span>
              </TooltipTrigger>
              <TooltipContent>草稿配置与生效配置一致</TooltipContent>
            </Tooltip>
          ))}

        <Separator orientation="vertical" className="h-6" />

        {/* 当前用户 / 角色 / 下拉菜单 */}
        <DropdownMenu>
          <DropdownMenuTrigger asChild>
            <Button variant="ghost" className="h-9 gap-2 px-2">
              <Avatar className="size-7">
                <AvatarFallback className="bg-primary text-xs text-primary-foreground">
                  {initials}
                </AvatarFallback>
              </Avatar>
              <span className="flex flex-col items-start leading-tight">
                <span className="text-xs">{userName}</span>
                <span className="text-[11px] text-muted-foreground">{ROLE_LABELS[role]}</span>
              </span>
              <ChevronDown className="size-3.5 text-muted-foreground" />
            </Button>
          </DropdownMenuTrigger>
          <DropdownMenuContent align="end" className="w-52">
            <DropdownMenuLabel>
              <div className="text-sm">{userName}</div>
              <div className="text-xs text-muted-foreground">{ROLE_LABELS[role]}</div>
            </DropdownMenuLabel>
            <DropdownMenuSeparator />
            <DropdownMenuItem onClick={onOpenProfile}>
              <UserRound className="size-4" />
              个人信息
            </DropdownMenuItem>
            <DropdownMenuItem onClick={onLogout} className="text-destructive focus:text-destructive">
              <LogOut className="size-4" />
              退出登录
            </DropdownMenuItem>
          </DropdownMenuContent>
        </DropdownMenu>
      </div>
    </header>
  );
}
