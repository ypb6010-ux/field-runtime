import { useEffect, useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { AlertCircle, Loader2, Plus, RefreshCw, Pencil, KeyRound, Trash2 } from "lucide-react";
import { apiDelete, apiGet, apiPost, apiPut } from "../api";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Label } from "../components/ui/label";
import { Badge } from "../components/ui/badge";
import { Switch } from "../components/ui/switch";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../components/ui/select";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/ui/tabs";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";

interface UserRow {
  id: string;
  username: string;
  role_id: string;
  enabled: string;
  created_at: string;
  last_login_at: string;
}

interface RoleRow {
  id: string;
  description: string;
  perms: string;
  users: string;
  permissions: string[];
}

interface UserForm {
  username: string;
  role: string;
  password: string;
}

interface EditForm {
  id: string;
  username: string;
  role: string;
  enabled: boolean;
}

const EMPTY_USER: UserForm = { username: "", role: "", password: "" };

const formatTime = (value: string) => {
  if (!value) return "-";
  return /^\d+$/.test(value) ? new Date(+value * 1000).toLocaleString() : value;
};

export function UsersRoles({ canWrite = true }: { canWrite?: boolean }) {
  const [users, setUsers] = useState<UserRow[]>([]);
  const [roles, setRoles] = useState<RoleRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [createOpen, setCreateOpen] = useState(false);
  const [createForm, setCreateForm] = useState<UserForm>(EMPTY_USER);
  const [editForm, setEditForm] = useState<EditForm | null>(null);
  const [del, setDel] = useState<UserRow | null>(null);
  const [passwordUser, setPasswordUser] = useState<UserRow | null>(null);
  const [newPassword, setNewPassword] = useState("");

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [nextUsers, nextRoles] = await Promise.all([apiGet<UserRow[]>("/users"), apiGet<RoleRow[]>("/roles")]);
      setUsers(nextUsers);
      setRoles(nextRoles);
    } catch (e) {
      setError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => { load(); }, []);

  const roleName = (id: string) => {
    const role = roles.find((r) => r.id === id);
    return role ? `${role.id}${role.description ? ` ${role.description}` : ""}` : id;
  };

  const roleOptions = useMemo(() => roles.map((r) => r.id), [roles]);

  function openCreate() {
    setCreateForm({ ...EMPTY_USER, role: roles[0]?.id ?? "" });
    setCreateOpen(true);
  }

  function openEdit(u: UserRow) {
    setEditForm({ id: u.id, username: u.username, role: u.role_id, enabled: u.enabled === "1" });
  }

  async function createUser() {
    try {
      await apiPost("/users", createForm);
      setCreateOpen(false);
      toast.success("已新增用户");
      await load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "新增失败");
    }
  }

  async function saveUser() {
    if (!editForm) return;
    try {
      await apiPut(`/users/${encodeURIComponent(editForm.id)}`, { role: editForm.role, enabled: editForm.enabled ? 1 : 0 });
      setEditForm(null);
      toast.success("已保存用户");
      await load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "保存失败");
    }
  }

  async function deleteUser() {
    if (!del) return;
    try {
      await apiDelete(`/users/${encodeURIComponent(del.id)}`);
      setDel(null);
      toast.success("已删除用户");
      await load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "删除失败");
    }
  }

  async function resetPassword() {
    if (!passwordUser) return;
    if (newPassword.length < 8 || newPassword.length > 256) {
      toast.error("密码长度必须为 8..256 个字符");
      return;
    }
    try {
      await apiPut(`/users/${encodeURIComponent(passwordUser.id)}/password`, {
        password: newPassword,
      });
      toast.success(`已重置「${passwordUser.username}」的密码，现有会话已失效`);
      setPasswordUser(null);
      setNewPassword("");
    } catch (resetError) {
      toast.error(resetError instanceof Error ? resetError.message : "密码重置失败");
    }
  }

  return (
    <>
      <PageHeader
        title="用户与角色"
        en="Users & Roles"
        description="管理用户、角色和按钮级权限。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}><RefreshCw className="size-3.5" />刷新</Button>
            <PermissionButton allowed={canWrite} onAction={openCreate} size="sm"><Plus className="size-3.5" />新增用户</PermissionButton>
          </>
        }
      />

      <div className="p-6">
        <Tabs defaultValue="users">
          <TabsList>
            <TabsTrigger value="users">Users 用户</TabsTrigger>
            <TabsTrigger value="roles">Roles 角色</TabsTrigger>
            <TabsTrigger value="matrix">Permission Matrix 权限矩阵</TabsTrigger>
          </TabsList>

          <TabsContent value="users" className="mt-4">
            <div className="rounded-md border border-border bg-card">
              {loading ? (
                <LoadingState />
              ) : error ? (
                <ErrorState message={error} onRetry={load} />
              ) : (
                <Table>
                  <TableHeader>
                    <TableRow>
                      <TableHead>用户名</TableHead><TableHead>角色</TableHead>
                      <TableHead>状态</TableHead><TableHead>最近登录</TableHead><TableHead>创建时间</TableHead>
                      <TableHead className="text-right">操作</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {users.map((u) => (
                      <TableRow key={u.id}>
                        <TableCell className="font-mono text-xs">{u.username}</TableCell>
                        <TableCell>{roleName(u.role_id)}</TableCell>
                        <TableCell>
                          <Badge variant="outline" className={u.enabled === "1" ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-muted text-muted-foreground border-border"}>
                            {u.enabled === "1" ? "启用" : "禁用"}
                          </Badge>
                        </TableCell>
                        <TableCell className="text-xs text-muted-foreground">{formatTime(u.last_login_at)}</TableCell>
                        <TableCell className="text-xs text-muted-foreground">{formatTime(u.created_at)}</TableCell>
                        <TableCell className="text-right">
                          <div className="flex justify-end gap-1">
                            <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => openEdit(u)}><Pencil className="size-3.5" /></Button>
                            <Button variant="ghost" size="icon" className="size-7" title="重置密码" disabled={!canWrite} onClick={() => { setPasswordUser(u); setNewPassword(""); }}><KeyRound className="size-3.5" /></Button>
                            <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(u)}><Trash2 className="size-3.5" /></Button>
                          </div>
                        </TableCell>
                      </TableRow>
                    ))}
                    {users.length === 0 && <TableRow><TableCell colSpan={6} className="h-24 text-center text-muted-foreground">暂无用户</TableCell></TableRow>}
                  </TableBody>
                </Table>
              )}
            </div>
          </TabsContent>

          <TabsContent value="roles" className="mt-4">
            <div className="rounded-md border border-border bg-card">
              {loading ? (
                <LoadingState />
              ) : error ? (
                <ErrorState message={error} onRetry={load} />
              ) : (
                <Table>
                  <TableHeader>
                    <TableRow>
                      <TableHead>角色名称</TableHead><TableHead>描述</TableHead><TableHead>用户数</TableHead>
                      <TableHead>权限数</TableHead><TableHead className="text-right">操作</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {roles.map((r) => (
                      <TableRow key={r.id}>
                        <TableCell className="font-medium">{r.id}</TableCell>
                        <TableCell className="text-muted-foreground">{r.description}</TableCell>
                        <TableCell>{+r.users || 0}</TableCell>
                        <TableCell>{+r.perms || r.permissions?.length || 0}</TableCell>
                        <TableCell className="text-right"><span className="text-xs text-muted-foreground">内置角色</span></TableCell>
                      </TableRow>
                    ))}
                    {roles.length === 0 && <TableRow><TableCell colSpan={5} className="h-24 text-center text-muted-foreground">暂无角色</TableCell></TableRow>}
                  </TableBody>
                </Table>
              )}
            </div>
          </TabsContent>

          <TabsContent value="matrix" className="mt-4">
            <div className="grid gap-3 md:grid-cols-3">
              {roles.map((role) => (
                <div key={role.id} className="rounded-md border border-border bg-card p-4">
                  <div className="font-medium">{role.id}</div>
                  <div className="mt-1 text-xs text-muted-foreground">{role.description}</div>
                  <div className="mt-3 flex flex-wrap gap-1.5">
                    {(role.permissions ?? []).map((permission) => (
                      <Badge key={permission} variant="outline" className="font-mono text-[11px]">
                        {permission}
                      </Badge>
                    ))}
                  </div>
                </div>
              ))}
            </div>
            <p className="mt-2 text-xs text-muted-foreground">
              权限矩阵由后端 role_permissions 返回；当前版本使用三种内置角色，不在界面中伪造可编辑状态。
            </p>
          </TabsContent>
        </Tabs>
      </div>

      <Dialog open={createOpen} onOpenChange={setCreateOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>新增用户</DialogTitle>
            <DialogDescription>创建后用户可按所选角色登录系统。</DialogDescription>
          </DialogHeader>
          <div className="space-y-3">
            <Field label="用户名"><Input value={createForm.username} onChange={(e) => setCreateForm((f) => ({ ...f, username: e.target.value }))} /></Field>
            <Field label="角色">
              <Select value={createForm.role} onValueChange={(v) => setCreateForm((f) => ({ ...f, role: v }))}>
                <SelectTrigger><SelectValue placeholder="选择角色" /></SelectTrigger>
                <SelectContent>{roleOptions.map((r) => <SelectItem key={r} value={r}>{roleName(r)}</SelectItem>)}</SelectContent>
              </Select>
            </Field>
            <Field label="初始密码"><Input type="password" minLength={8} maxLength={256} value={createForm.password} onChange={(e) => setCreateForm((f) => ({ ...f, password: e.target.value }))} placeholder="8–256 个字符" /></Field>
          </div>
          <DialogFooter>
            <Button variant="outline" onClick={() => setCreateOpen(false)}>取消</Button>
            <Button onClick={createUser} disabled={!createForm.username || !createForm.role || createForm.password.length < 8 || createForm.password.length > 256}>确认新增</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={!!editForm} onOpenChange={(o) => !o && setEditForm(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>编辑用户</DialogTitle>
            <DialogDescription>修改用户「{editForm?.username}」的角色与启用状态。</DialogDescription>
          </DialogHeader>
          {editForm && (
            <div className="space-y-3">
              <Field label="角色">
                <Select value={editForm.role} onValueChange={(v) => setEditForm((f) => f ? { ...f, role: v } : f)}>
                  <SelectTrigger><SelectValue placeholder="选择角色" /></SelectTrigger>
                  <SelectContent>{roleOptions.map((r) => <SelectItem key={r} value={r}>{roleName(r)}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label="启用"><Switch checked={editForm.enabled} onCheckedChange={(v) => setEditForm((f) => f ? { ...f, enabled: v } : f)} /></Field>
            </div>
          )}
          <DialogFooter>
            <Button variant="outline" onClick={() => setEditForm(null)}>取消</Button>
            <Button onClick={saveUser} disabled={!editForm?.role}>保存</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除用户？</DialogTitle>
            <DialogDescription>删除后用户「{del?.username}」将无法登录，该操作会写入审计日志。</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setDel(null)}>取消</Button>
            <Button variant="destructive" onClick={deleteUser}>确认删除</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={!!passwordUser} onOpenChange={(open) => !open && setPasswordUser(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>重置用户密码</DialogTitle>
            <DialogDescription>
              为「{passwordUser?.username}」设置新密码。成功后该用户的所有现有会话会立即失效。
            </DialogDescription>
          </DialogHeader>
          <Field label="新密码">
            <Input
              type="password"
              minLength={8}
              maxLength={256}
              value={newPassword}
              onChange={(event) => setNewPassword(event.target.value)}
              placeholder="8–256 个字符"
            />
          </Field>
          <DialogFooter>
            <Button variant="outline" onClick={() => setPasswordUser(null)}>取消</Button>
            <Button onClick={resetPassword} disabled={newPassword.length < 8 || newPassword.length > 256}>确认重置</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}

function LoadingState() {
  return <div className="flex items-center justify-center gap-2 py-20 text-muted-foreground"><Loader2 className="size-4 animate-spin" />加载中…</div>;
}

function ErrorState({ message, onRetry }: { message: string; onRetry: () => void }) {
  return (
    <div className="flex flex-col items-center gap-3 py-16 text-center">
      <AlertCircle className="size-7 text-red-500" />
      <div className="text-sm font-medium">加载失败</div>
      <div className="text-sm text-muted-foreground">{message}</div>
      <Button variant="outline" size="sm" onClick={onRetry}>重试</Button>
    </div>
  );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[96px_1fr] items-center gap-3">
      <Label className="text-sm text-muted-foreground">{label}</Label>
      <div>{children}</div>
    </div>
  );
}
