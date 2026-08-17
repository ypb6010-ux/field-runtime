import type { LucideIcon } from "lucide-react";

/** 系统三种角色 */
export type Role = "viewer" | "operator" | "admin";

export const ROLE_LABELS: Record<Role, string> = {
  viewer: "Viewer 只读",
  operator: "Operator 操作员",
  admin: "Admin 管理员",
};

/** 全部页面 key（IA 节点） */
export type PageKey =
  | "dashboard"
  | "protocols"
  | "datapoints"
  | "polling"
  | "conversion"
  | "control"
  | "live"
  | "history"
  | "config"
  | "settings"
  | "apidocs"
  | "logs"
  | "users";

/** 连接 / WebSocket 等运行态状态。info=检查中/中性蓝 */
export type StatusTone = "success" | "warning" | "error" | "draft" | "disabled" | "info";

/** 后端 /auth/me 返回的权限标识 */
export type Permission =
  | "config:read"
  | "config:write"
  | "conversion:write"
  | "protocol:write"
  | "user:write";

/** 后端 /auth/me 返回的当前用户 */
export interface AuthUser {
  name: string;
  role: Role;
  permissions: Permission[];
}

export interface NavItem {
  key: PageKey;
  label: string;
  en: string;
  icon: LucideIcon;
  /** 允许访问该页面的角色 */
  roles: Role[];
  /** 该页面是否需要 config 权限（影响“未生效项”徽标可见性） */
  configScoped?: boolean;
}

export interface NavGroup {
  id: string;
  title: string;
  items: NavItem[];
}
