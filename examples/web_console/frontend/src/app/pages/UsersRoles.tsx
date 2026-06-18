import { useEffect, useMemo, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { AlertCircle, Loader2, Plus, RefreshCw, Pencil, KeyRound, Trash2, ShieldAlert } from "lucide-react";
import { apiDelete, apiGet, apiPost, apiPut } from "../api";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Label } from "../components/ui/label";
import { Badge } from "../components/ui/badge";
import { Checkbox } from "../components/ui/checkbox";
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

const MATRIX_ROWS = ["Dashboard", "Protocols", "Datapoints", "Polling", "Conversion", "Live", "History", "Config & Apply", "Settings", "Logs", "Users & Roles"];
const MATRIX_COLS = ["View", "Create", "Edit", "Delete", "Apply", "Rollback", "Control", "Export"];
const HIGH_RISK = new Set(["Apply", "Rollback", "Delete"]);

const formatTime = (value: string) => {
  if (!value) return "-";
  return /^\d+$/.test(value) ? new Date(+value).toLocaleString() : value;
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
      await apiPut(`/users/${editForm.id}`, { role: editForm.role, enabled: editForm.enabled ? 1 : 0 });
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
      await apiDelete(`/users/${del.id}`);
      setDel(null);
      toast.success("已删除用户");
      await load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "删除失败");
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
            <PermissionButton allowed={canWrite} onAction={() => toast.message("新增角色（演示）")} size="sm" variant="outline"><Plus className="size-3.5" />新增角色</PermissionButton>
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
                            <Button variant="ghost" size="icon" className="size-7" title="重置密码" disabled={!canWrite} onClick={() => toast.message(`重置「${u.username}」密码（演示）`)}><KeyRound className="size-3.5" /></Button>
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
                        <TableCell className="text-right"><Button variant="ghost" size="sm" disabled={!canWrite} onClick={() => toast.message(`编辑角色「${r.id}」（演示）`)}>编辑权限</Button></TableCell>
                      </TableRow>
                    ))}
                    {roles.length === 0 && <TableRow><TableCell colSpan={5} className="h-24 text-center text-muted-foreground">暂无角色</TableCell></TableRow>}
                  </TableBody>
                </Table>
              )}
            </div>
          </TabsContent>

          <TabsContent value="matrix" className="mt-4">
            <div className="overflow-x-auto rounded-md border border-border bg-card">
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead className="sticky left-0 bg-card">页面 / 权限</TableHead>
                    {MATRIX_COLS.map((c) => (
                      <TableHead key={c} className="text-center">
                        <span className="inline-flex items-center gap-1">{c}{HIGH_RISK.has(c) && <ShieldAlert className="size-3 text-amber-500" />}</span>
                      </TableHead>
                    ))}
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {MATRIX_ROWS.map((row) => (
                    <TableRow key={row}>
                      <TableCell className="sticky left-0 bg-card font-medium">{row}</TableCell>
                      {MATRIX_COLS.map((col) => (
                        <TableCell key={col} className="text-center">
                          <Checkbox defaultChecked={col === "View" || (row !== "Users & Roles" && ["Create", "Edit"].includes(col))} disabled={!canWrite} className={HIGH_RISK.has(col) ? "data-[state=checked]:bg-amber-500 data-[state=checked]:border-amber-500" : ""} />
                        </TableCell>
                      ))}
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
            <p className="mt-2 text-xs text-muted-foreground"><ShieldAlert className="mr-1 inline size-3 text-amber-500" />高风险权限（Apply / Rollback / Delete / 系统维护）需谨慎授予。</p>
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
            <Field label="初始密码"><Input type="password" value={createForm.password} onChange={(e) => setCreateForm((f) => ({ ...f, password: e.target.value }))} /></Field>
          </div>
          <DialogFooter>
            <Button variant="outline" onClick={() => setCreateOpen(false)}>取消</Button>
            <Button onClick={createUser} disabled={!createForm.username || !createForm.role || !createForm.password}>确认新增</Button>
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
