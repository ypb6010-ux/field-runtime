import {
  LayoutDashboard,
  Network,
  Radar,
  Timer,
  Shuffle,
  Activity,
  History,
  GitPullRequestArrow,
  Settings,
  BookText,
  ScrollText,
  Users,
} from "lucide-react";
import type { NavGroup, Role, NavItem, PageKey } from "./types";

/**
 * 信息架构 (IA) —— 侧边导航按业务域分组。
 * roles 决定该节点对哪些角色可见，实现角色导航差异。
 */
export const NAV_GROUPS: NavGroup[] = [
  {
    id: "monitor",
    title: "监控 Monitor",
    items: [
      {
        key: "dashboard",
        label: "概览",
        en: "Dashboard",
        icon: LayoutDashboard,
        roles: ["viewer", "operator", "admin"],
      },
      {
        key: "live",
        label: "实时监控",
        en: "Live",
        icon: Activity,
        roles: ["viewer", "operator", "admin"],
      },
      {
        key: "history",
        label: "历史数据",
        en: "History",
        icon: History,
        roles: ["viewer", "operator", "admin"],
      },
    ],
  },
  {
    id: "acquisition",
    title: "采集 Acquisition",
    items: [
      {
        key: "protocols",
        label: "协议管理",
        en: "Protocols",
        icon: Network,
        roles: ["operator", "admin"],
        configScoped: true,
      },
      {
        key: "datapoints",
        label: "采集点",
        en: "Datapoints",
        icon: Radar,
        roles: ["operator", "admin"],
        configScoped: true,
      },
      {
        key: "polling",
        label: "轮询任务",
        en: "Polling",
        icon: Timer,
        roles: ["operator", "admin"],
        configScoped: true,
      },
      {
        key: "conversion",
        label: "协议转换",
        en: "Conversion",
        icon: Shuffle,
        roles: ["operator", "admin"],
        configScoped: true,
      },
    ],
  },
  {
    id: "delivery",
    title: "发布 Delivery",
    items: [
      {
        key: "config",
        label: "配置发布",
        en: "Config & Apply",
        icon: GitPullRequestArrow,
        roles: ["admin"],
        configScoped: true,
      },
      {
        key: "apidocs",
        label: "API 文档",
        en: "API Docs",
        icon: BookText,
        roles: ["admin"],
      },
    ],
  },
  {
    id: "system",
    title: "系统 System",
    items: [
      {
        key: "logs",
        label: "事件日志",
        en: "Logs",
        icon: ScrollText,
        roles: ["admin"],
      },
      {
        key: "users",
        label: "用户与角色",
        en: "Users & Roles",
        icon: Users,
        roles: ["admin"],
      },
      {
        key: "settings",
        label: "系统设置",
        en: "Settings",
        icon: Settings,
        roles: ["admin"],
      },
    ],
  },
];

/** 扁平化的所有导航项，便于按 key 查找 */
export const ALL_ITEMS: NavItem[] = NAV_GROUPS.flatMap((g) => g.items);

export function getNavItem(key: PageKey): NavItem | undefined {
  return ALL_ITEMS.find((i) => i.key === key);
}

/** 当前角色可见的分组（过滤掉无权限的节点和空分组） */
export function visibleGroups(role: Role): NavGroup[] {
  return NAV_GROUPS.map((g) => ({
    ...g,
    items: g.items.filter((i) => i.roles.includes(role)),
  })).filter((g) => g.items.length > 0);
}

export function canAccess(role: Role, key: PageKey): boolean {
  const item = getNavItem(key);
  return !!item && item.roles.includes(role);
}

/** 是否拥有 config 权限（决定“未生效项”徽标可见性） */
export function hasConfigAccess(role: Role): boolean {
  return role !== "viewer";
}

/** 角色登录后的默认落地页（由后端/产品策略约定） */
export const ROLE_DEFAULT_PAGE: Record<Role, PageKey> = {
  viewer: "dashboard",
  operator: "live",
  admin: "dashboard",
};

export function defaultPage(role: Role): PageKey {
  return ROLE_DEFAULT_PAGE[role];
}
