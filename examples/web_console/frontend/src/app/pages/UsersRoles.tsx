import { useState } from "react";
import { toast } from "sonner";
import { Plus, RefreshCw, Pencil, KeyRound, Trash2, ShieldAlert } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Badge } from "../components/ui/badge";
import { Checkbox } from "../components/ui/checkbox";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/ui/tabs";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";

interface User { id: string; username: string; name: string; role: string; enabled: boolean; lastLogin: string; createdAt: string; }
const USERS: User[] = [
  { id: "u1", username: "admin", name: "张工", role: "Admin 管理员", enabled: true, lastLogin: "2026-06-18 10:02", createdAt: "2026-01-04", },
  { id: "u2", username: "operator", name: "李操", role: "Operator 操作员", enabled: true, lastLogin: "2026-06-18 08:31", createdAt: "2026-02-12", },
  { id: "u3", username: "viewer", name: "王看", role: "Viewer 只读", enabled: true, lastLogin: "2026-06-17 19:44", createdAt: "2026-03-01", },
  { id: "u4", username: "tempops", name: "临时账号", role: "Operator 操作员", enabled: false, lastLogin: "2026-05-20 14:10", createdAt: "2026-05-01", },
];

const ROLES = [
  { id: "admin", name: "Admin 管理员", desc: "全系统配置与维护", users: 1, perms: 24, updatedAt: "2026-06-10" },
  { id: "operator", name: "Operator 操作员", desc: "监控 + 控制写 + 转换管理", users: 2, perms: 12, updatedAt: "2026-06-08" },
  { id: "viewer", name: "Viewer 只读", desc: "只读监控", users: 1, perms: 7, updatedAt: "2026-05-30" },
];

const MATRIX_ROWS = ["Dashboard", "Protocols", "Datapoints", "Polling", "Conversion", "Live", "History", "Config & Apply", "Settings", "Logs", "Users & Roles"];
const MATRIX_COLS = ["View", "Create", "Edit", "Delete", "Apply", "Rollback", "Control", "Export"];
const HIGH_RISK = new Set(["Apply", "Rollback", "Delete"]);

export function UsersRoles({ canWrite = true }: { canWrite?: boolean }) {
  const [users, setUsers] = useState<User[]>(USERS);
  const [del, setDel] = useState<User | null>(null);

  return (
    <>
      <PageHeader
        title="用户与角色"
        en="Users & Roles"
        description="管理用户、角色和按钮级权限。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新")}><RefreshCw className="size-3.5" />刷新</Button>
            <PermissionButton allowed={canWrite} onAction={() => toast.message("新增角色（演示）")} size="sm" variant="outline"><Plus className="size-3.5" />新增角色</PermissionButton>
            <PermissionButton allowed={canWrite} onAction={() => toast.message("新增用户（演示）")} size="sm"><Plus className="size-3.5" />新增用户</PermissionButton>
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
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>用户名</TableHead><TableHead>显示名</TableHead><TableHead>角色</TableHead>
                    <TableHead>状态</TableHead><TableHead>最近登录</TableHead><TableHead>创建时间</TableHead>
                    <TableHead className="text-right">操作</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {users.map((u) => (
                    <TableRow key={u.id}>
                      <TableCell className="font-mono text-xs">{u.username}</TableCell>
                      <TableCell className="font-medium">{u.name}</TableCell>
                      <TableCell>{u.role}</TableCell>
                      <TableCell>
                        <Badge variant="outline" className={u.enabled ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-muted text-muted-foreground border-border"}>
                          {u.enabled ? "启用" : "禁用"}
                        </Badge>
                      </TableCell>
                      <TableCell className="text-xs text-muted-foreground">{u.lastLogin}</TableCell>
                      <TableCell className="text-xs text-muted-foreground">{u.createdAt}</TableCell>
                      <TableCell className="text-right">
                        <div className="flex justify-end gap-1">
                          <Button variant="ghost" size="icon" className="size-7" title="编辑" disabled={!canWrite} onClick={() => toast.message(`编辑「${u.name}」`)}><Pencil className="size-3.5" /></Button>
                          <Button variant="ghost" size="icon" className="size-7" title="重置密码" disabled={!canWrite} onClick={() => toast.message(`重置「${u.name}」密码`)}><KeyRound className="size-3.5" /></Button>
                          <Button variant="ghost" size="icon" className="size-7 text-red-600" title="删除" disabled={!canWrite} onClick={() => setDel(u)}><Trash2 className="size-3.5" /></Button>
                        </div>
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
          </TabsContent>

          <TabsContent value="roles" className="mt-4">
            <div className="rounded-md border border-border bg-card">
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>角色名称</TableHead><TableHead>描述</TableHead><TableHead>用户数</TableHead>
                    <TableHead>权限数</TableHead><TableHead>更新时间</TableHead><TableHead className="text-right">操作</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {ROLES.map((r) => (
                    <TableRow key={r.id}>
                      <TableCell className="font-medium">{r.name}</TableCell>
                      <TableCell className="text-muted-foreground">{r.desc}</TableCell>
                      <TableCell>{r.users}</TableCell>
                      <TableCell>{r.perms}</TableCell>
                      <TableCell className="text-xs text-muted-foreground">{r.updatedAt}</TableCell>
                      <TableCell className="text-right"><Button variant="ghost" size="sm" disabled={!canWrite} onClick={() => toast.message(`编辑角色「${r.name}」`)}>编辑权限</Button></TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
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

      <Dialog open={!!del} onOpenChange={(o) => !o && setDel(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认删除用户？</DialogTitle>
            <DialogDescription>删除后用户「{del?.name}」将无法登录，该操作会写入审计日志。</DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setDel(null)}>取消</Button>
            <Button variant="destructive" onClick={() => { setUsers((p) => p.filter((x) => x.id !== del?.id)); setDel(null); toast.success("已删除用户"); }}>确认删除</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}
