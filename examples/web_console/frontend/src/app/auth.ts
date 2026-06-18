import type { AuthUser, Permission, Role } from "./types";
import { apiPost, setToken } from "./api";

/**
 * 鉴权：POST /auth/login → { accessToken, user{role,permissions} }，token 存 localStorage。
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

// 后端权限位 → 前端 Permission 模型（前端按钮级权限）。
function mapPermissions(backend: string[]): Permission[] {
  const out = new Set<Permission>();
  for (const p of backend) {
    if (p === "config:read") out.add("config:read");
    if (p === "config:write") {
      out.add("config:write");
      out.add("protocol:write");
    }
    if (p === "conversion:manage") out.add("conversion:write");
    if (p === "user:manage") out.add("user:write");
  }
  return [...out];
}

interface LoginResp {
  accessToken: string;
  user: { username: string; role: string; permissions: string[] };
}

export async function login(username: string, password: string): Promise<AuthUser> {
  const u = username.trim();
  if (!u || !password) throw new AuthError("empty", "请输入账号和密码。");
  let resp: LoginResp;
  try {
    resp = await apiPost<LoginResp>("/auth/login", { username: u, password });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "登录失败";
    if (/连接|HTTP|网络/.test(msg)) {
      throw new AuthError("unreachable", "无法连接服务器，请检查后端服务或稍后重试。");
    }
    throw new AuthError("invalid", "账号或密码错误。");
  }
  setToken(resp.accessToken);
  const role = (["viewer", "operator", "admin"].includes(resp.user.role)
    ? resp.user.role
    : "viewer") as Role;
  return {
    name: resp.user.username,
    role,
    permissions: mapPermissions(resp.user.permissions),
  };
}

export function hasPermission(user: Pick<AuthUser, "permissions">, perm: Permission): boolean {
  return user.permissions.includes(perm);
}
