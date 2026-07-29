import { useEffect, useState } from "react";
import { AlertCircle, DatabaseZap, Loader2, RefreshCw, RotateCcw, Save } from "lucide-react";
import { toast } from "sonner";
import { apiGet, apiPost, apiPut } from "../api";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Button } from "../components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "../components/ui/card";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "../components/ui/dialog";
import { Input } from "../components/ui/input";
import { Label } from "../components/ui/label";

interface SettingsResponse {
  sample_retention_days?: number | string;
}

export function Settings({ canWrite = true }: { canWrite?: boolean }) {
  const [retentionDays, setRetentionDays] = useState(30);
  const [originalDays, setOriginalDays] = useState(30);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [cleanupOpen, setCleanupOpen] = useState(false);
  const [confirmText, setConfirmText] = useState("");

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const settings = await apiGet<SettingsResponse>("/system/settings");
      const parsed = Number(settings.sample_retention_days ?? 30);
      const value = Number.isInteger(parsed) && parsed >= 1 && parsed <= 3650
        ? parsed
        : 30;
      setRetentionDays(value);
      setOriginalDays(value);
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : "设置加载失败");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
  }, []);

  async function save() {
    if (!Number.isInteger(retentionDays) || retentionDays < 1 || retentionDays > 3650) {
      toast.error("采样保留期必须是 1..3650 天的整数");
      return;
    }
    if (retentionDays === originalDays) {
      toast.message("没有需要保存的设置");
      return;
    }
    setSaving(true);
    try {
      await apiPut("/system/settings", {
        sample_retention_days: retentionDays,
      });
      setOriginalDays(retentionDays);
      toast.success("采样保留期已更新");
    } catch (saveError) {
      toast.error(saveError instanceof Error ? saveError.message : "保存失败");
    } finally {
      setSaving(false);
    }
  }

  async function cleanup() {
    setSaving(true);
    try {
      const result = await apiPost<{ deleted: number }>(
        "/system/maintenance/cleanup-samples",
      );
      setCleanupOpen(false);
      setConfirmText("");
      toast.success(`清理完成，共删除 ${result.deleted} 条过期采样`);
    } catch (cleanupError) {
      toast.error(cleanupError instanceof Error ? cleanupError.message : "清理失败");
    } finally {
      setSaving(false);
    }
  }

  return (
    <>
      <PageHeader
        title="系统设置"
        en="Settings"
        description="管理已由后端实际执行的运行设置。"
        actions={
          <>
            <Button variant="ghost" size="sm" onClick={load} disabled={loading || saving}>
              <RefreshCw className="size-3.5" />刷新
            </Button>
            <Button
              variant="outline"
              size="sm"
              onClick={() => setRetentionDays(originalDays)}
              disabled={loading || saving || retentionDays === originalDays}
            >
              <RotateCcw className="size-3.5" />撤销修改
            </Button>
            <PermissionButton allowed={canWrite} onAction={save} size="sm" disabled={loading || saving}>
              {saving ? <Loader2 className="size-3.5 animate-spin" /> : <Save className="size-3.5" />}
              保存
            </PermissionButton>
          </>
        }
      />

      <div className="p-6">
        {loading ? (
          <div className="flex items-center justify-center gap-2 rounded-lg border bg-card py-20 text-muted-foreground">
            <Loader2 className="size-4 animate-spin" />加载中…
          </div>
        ) : error ? (
          <div className="flex flex-col items-center gap-3 rounded-lg border border-dashed bg-card py-16 text-center">
            <AlertCircle className="size-7 text-destructive" />
            <div className="font-medium">加载失败</div>
            <div className="text-sm text-muted-foreground">{error}</div>
            <Button variant="outline" size="sm" onClick={load}>重试</Button>
          </div>
        ) : (
          <div className="grid max-w-4xl gap-4 lg:grid-cols-2">
            <Card className="rounded-lg">
              <CardHeader>
                <CardTitle className="text-sm">历史采样保留</CardTitle>
              </CardHeader>
              <CardContent className="space-y-4">
                <div className="space-y-1.5">
                  <Label htmlFor="retention-days">保留天数</Label>
                  <div className="flex items-center gap-2">
                    <Input
                      id="retention-days"
                      type="number"
                      min={1}
                      max={3650}
                      value={retentionDays}
                      onChange={(event) => setRetentionDays(Number(event.target.value))}
                    />
                    <span className="text-sm text-muted-foreground">天</span>
                  </div>
                </div>
                <div className="rounded-md border bg-muted/30 p-3 text-xs leading-relaxed text-muted-foreground">
                  后端每小时自动删除超出保留期的 samples 记录。修改后无需重启运行时。
                </div>
              </CardContent>
            </Card>

            <Card className="rounded-lg border-amber-200">
              <CardHeader>
                <CardTitle className="text-sm text-amber-700">立即清理</CardTitle>
              </CardHeader>
              <CardContent className="space-y-4">
                <p className="text-sm text-muted-foreground">
                  立即按当前已保存的保留期清理过期采样。删除后的历史数据无法恢复。
                </p>
                <Button
                  variant="destructive"
                  size="sm"
                  disabled={!canWrite || saving}
                  onClick={() => setCleanupOpen(true)}
                >
                  <DatabaseZap className="size-4" />清理过期采样
                </Button>
              </CardContent>
            </Card>
          </div>
        )}
      </div>

      <Dialog open={cleanupOpen} onOpenChange={setCleanupOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>确认清理过期采样？</DialogTitle>
            <DialogDescription>
              将按已保存的 {originalDays} 天保留期永久删除更早的数据。
            </DialogDescription>
          </DialogHeader>
          <Input
            value={confirmText}
            onChange={(event) => setConfirmText(event.target.value)}
            placeholder="输入 CLEANUP 确认"
            className="font-mono"
          />
          <DialogFooter>
            <Button variant="outline" onClick={() => setCleanupOpen(false)}>取消</Button>
            <Button
              variant="destructive"
              onClick={cleanup}
              disabled={confirmText !== "CLEANUP" || saving}
            >
              {saving && <Loader2 className="size-4 animate-spin" />}确认清理
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}
