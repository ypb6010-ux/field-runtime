import { useState, type ReactNode } from "react";
import { toast } from "sonner";
import { RefreshCw, RotateCcw, Save, Trash2, Power, FileWarning, DatabaseZap } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Badge } from "../components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "../components/ui/card";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../components/ui/select";
import {
  Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter,
} from "../components/ui/dialog";
import { Label } from "../components/ui/label";
import { Switch } from "../components/ui/switch";
import { Separator } from "../components/ui/separator";

type SettingStatus = "normal" | "warning" | "danger";

interface MaintenanceOp {
  id: string;
  title: string;
  description: string;
  icon: ReactNode;
  status: SettingStatus;
}

const STATUS_META: Record<SettingStatus, { label: string; cls: string }> = {
  normal: { label: "可执行", cls: "bg-sky-50 text-sky-700 border-sky-200" },
  warning: { label: "需确认", cls: "bg-amber-50 text-amber-700 border-amber-200" },
  danger: { label: "高风险", cls: "bg-red-50 text-red-700 border-red-200" },
};

const MAINTENANCE_OPS: MaintenanceOp[] = [
  { id: "clear-history", title: "清理历史数据", description: "按数据保留期删除过期历史采集数据。", icon: <DatabaseZap className="size-4" />, status: "danger" },
  { id: "clear-events", title: "清理事件日志", description: "清理超过保留期的系统事件和审计日志。", icon: <Trash2 className="size-4" />, status: "warning" },
  { id: "restart-runtime", title: "重启运行时", description: "短暂中断实时采集和 WebSocket 连接。", icon: <Power className="size-4" />, status: "danger" },
  { id: "reload-config", title: "重新加载配置", description: "从 active config 重新加载运行配置。", icon: <FileWarning className="size-4" />, status: "warning" },
];

export function Settings({ canWrite = true }: { canWrite?: boolean }) {
  const [op, setOp] = useState<MaintenanceOp | null>(null);
  const [confirmText, setConfirmText] = useState("");

  function openOp(next: MaintenanceOp) {
    setOp(next);
    setConfirmText("");
  }

  function confirmOp() {
    if (!op) return;
    toast.success(`已提交维护操作：${op.title}`);
    setOp(null);
    setConfirmText("");
  }

  return (
    <>
      <PageHeader
        title="系统设置"
        en="Settings"
        description="管理日志级别、数据保留、WebSocket 与维护操作。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新系统设置")}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!canWrite} onClick={() => toast.message("已重置为当前生效设置")}>
              <RotateCcw className="size-3.5" />重置
            </Button>
            <PermissionButton allowed={canWrite} onAction={() => toast.success("已保存设置，部分项需重启运行时后生效")} size="sm">
              <Save className="size-3.5" />保存设置
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="grid gap-4 xl:grid-cols-2">
          <Card className="rounded-md">
            <CardHeader className="px-4 pt-4">
              <CardTitle className="text-sm">基础设置</CardTitle>
            </CardHeader>
            <CardContent className="space-y-3 px-4 pb-4">
              <Field label="系统名称"><Input defaultValue="IDC Gateway 工业采集平台" /></Field>
              <Field label="时区">
                <Select defaultValue="Asia/Shanghai">
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="Asia/Shanghai">Asia/Shanghai</SelectItem><SelectItem value="UTC">UTC</SelectItem><SelectItem value="Asia/Singapore">Asia/Singapore</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="语言">
                <Select defaultValue="zh-CN">
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="zh-CN">简体中文</SelectItem><SelectItem value="en-US">English</SelectItem></SelectContent>
                </Select>
              </Field>
            </CardContent>
          </Card>

          <Card className="rounded-md">
            <CardHeader className="px-4 pt-4">
              <CardTitle className="text-sm">日志设置</CardTitle>
            </CardHeader>
            <CardContent className="space-y-3 px-4 pb-4">
              <Field label="日志级别">
                <Select defaultValue="info">
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent><SelectItem value="debug">debug</SelectItem><SelectItem value="info">info</SelectItem><SelectItem value="warning">warning</SelectItem><SelectItem value="error">error</SelectItem></SelectContent>
                </Select>
              </Field>
              <Field label="日志保留期"><Input defaultValue="30 天" /></Field>
              <Field label="审计日志保留期"><Input defaultValue="180 天" /></Field>
            </CardContent>
          </Card>

          <Card className="rounded-md">
            <CardHeader className="px-4 pt-4">
              <CardTitle className="text-sm">数据保留</CardTitle>
            </CardHeader>
            <CardContent className="space-y-3 px-4 pb-4">
              <Field label="实时缓存时间"><Input defaultValue="10 分钟" /></Field>
              <Field label="历史数据保留期"><Input defaultValue="365 天" /></Field>
              <Field label="自动清理"><Switch defaultChecked /></Field>
            </CardContent>
          </Card>

          <Card className="rounded-md">
            <CardHeader className="px-4 pt-4">
              <CardTitle className="text-sm">WebSocket 设置</CardTitle>
            </CardHeader>
            <CardContent className="space-y-3 px-4 pb-4">
              <Field label="心跳间隔"><Input defaultValue="15s" /></Field>
              <Field label="重连间隔"><Input defaultValue="3s" /></Field>
              <Field label="最大重试次数"><Input defaultValue="10" /></Field>
              <Field label="latest-wins"><Switch defaultChecked /></Field>
              <div className="rounded-md border border-border bg-muted/40 p-3 text-xs text-muted-foreground">
                latest-wins 开启后，同一连接只保留最新实时值，降低高频采集时的前端堆积。
              </div>
            </CardContent>
          </Card>
        </div>

        <Card className="rounded-md border-red-200">
          <CardHeader className="px-4 pt-4">
            <CardTitle className="text-sm text-red-700">维护操作</CardTitle>
          </CardHeader>
          <CardContent className="space-y-3 px-4 pb-4">
            <div className="rounded-md border border-red-200 bg-red-50 p-3 text-sm text-red-700">
              危险操作可能影响实时采集和 WebSocket 连接，仅当前用户 张工 / Admin 可执行。
            </div>
            <div className="grid gap-3 md:grid-cols-2">
              {MAINTENANCE_OPS.map((item) => (
                <div key={item.id} className="rounded-md border border-border bg-card p-3">
                  <div className="flex items-start justify-between gap-3">
                    <div className="flex items-center gap-2">
                      <span className="text-red-600">{item.icon}</span>
                      <div className="font-medium">{item.title}</div>
                    </div>
                    <Badge variant="outline" className={STATUS_META[item.status].cls}>{STATUS_META[item.status].label}</Badge>
                  </div>
                  <div className="mt-2 text-sm text-muted-foreground">{item.description}</div>
                  <Separator className="my-3" />
                  <Button variant={item.status === "danger" ? "destructive" : "outline"} size="sm" disabled={!canWrite} onClick={() => openOp(item)}>
                    执行
                  </Button>
                </div>
              ))}
            </div>
          </CardContent>
        </Card>
      </div>

      <Dialog open={!!op} onOpenChange={(o) => !o && setOp(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认执行维护操作？</DialogTitle>
            <DialogDescription>该操作可能影响实时采集和 WebSocket 连接，请确认当前无关键任务。</DialogDescription>
          </DialogHeader>
          <div className="space-y-4">
            <div className="rounded-md border border-red-200 bg-red-50 p-3">
              <div className="flex items-center justify-between gap-2">
                <div className="font-medium text-red-700">{op?.title}</div>
                {op && <Badge variant="outline" className={STATUS_META[op.status].cls}>{STATUS_META[op.status].label}</Badge>}
              </div>
              <div className="mt-1 text-sm text-red-700/80">{op?.description}</div>
            </div>
            <Field label="二次确认">
              <Input value={confirmText} onChange={(e) => setConfirmText(e.target.value)} placeholder="输入 EXECUTE 后确认执行" />
            </Field>
          </div>
          <DialogFooter>
            <Button variant="outline" onClick={() => setOp(null)}>取消</Button>
            <Button variant="destructive" disabled={confirmText !== "EXECUTE"} onClick={confirmOp}>确认执行</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[128px_1fr] items-center gap-3">
      <Label className="text-sm text-muted-foreground">{label}</Label>
      <div>{children}</div>
    </div>
  );
}
