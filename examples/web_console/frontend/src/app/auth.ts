import type { AuthUser, Permission, Role } from "./types";

/**
 * 模拟后端鉴权。真实环境中：
 *   POST /auth/login  → 设置会话
 *   GET  /auth/me     → 返回 { name, role, permissions }
 * 角色与权限由后端决定，前端不可选择。
 */

export type AuthErrorCode = "empty" | "invalid" | "unreachable" | "forbidden";

export class AuthError extends Error {
  code: AuthErrorCode;
  constructor(code: AuthErrorCode, message: string) {
    super(message);
    this.code = code;
  }
}

const ROLE_PERMISSIONS: Record<Role, Permission[]> = {
  viewer: [],
  operator: ["conversion:write"],
  admin: ["config:read", "config:write", "conversion:write", "protocol:write", "user:write"],
};

const DISPLAY_NAME: Record<Role, string> = {
  viewer: "李巡检",
  operator: "王操作",
  admin: "张工",
};

// 演示账号 → 角色（真实环境由后端账号体系决定）
const DEMO_ACCOUNTS: Record<string, Role> = {
  admin: "admin",
  operator: "operator",
  viewer: "viewer",
};

function delay(ms: number) {
  return new Promise((r) => setTimeout(r, ms));
}

/** 模拟登录 + 拉取 /auth/me。失败时抛出 AuthError。 */
export async function login(username: string, password: string): Promise<AuthUser> {
  const u = username.trim().toLowerCase();

  // 模拟网络往返
  await delay(900);

  // 触发“后端不可达”：账号填 offline
  if (u === "offline") {
    throw new AuthError("unreachable", "无法连接服务器，请检查网络或稍后重试。");
  }

  // 凭证校验：演示密码统一为 demo
  const role = DEMO_ACCOUNTS[u];
  if (!role || password !== "demo") {
    throw new AuthError("invalid", "账号或密码错误。");
  }

  return {
    name: DISPLAY_NAME[role],
    role,
    permissions: ROLE_PERMISSIONS[role],
  };
}

export function hasPermission(user: Pick<AuthUser, "permissions">, perm: Permission): boolean {
  return user.permissions.includes(perm);
}
