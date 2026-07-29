import { useEffect, useState } from "react";
import { toast } from "sonner";
import {
  CheckCircle2,
  FileCode2,
  History,
  Loader2,
  RefreshCw,
  RotateCcw,
  ShieldCheck,
  UploadCloud,
  XCircle,
} from "lucide-react";
import { apiGet, apiPost } from "../api";
import { PageHeader } from "../components/PageHeader";
import { Badge } from "../components/ui/badge";
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
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "../components/ui/table";

interface ConfigStatus {
  runtimeRunning: boolean;
  draftDirty: boolean;
  renderedToml: string;
  counts: Record<string, number>;
  activeVersion?: number;
  author?: string;
  note?: string;
  appliedAt?: number;
}

interface VersionRow {
  version: string;
  status: string;
  author: string;
  note: string;
  applied_at: string;
  created_at: string;
}

interface ApplyResult {
  applied?: boolean;
  rolledBack?: boolean;
  version?: number;
  versionRecorded?: boolean;
  warning?: string;
}

function formatTime(seconds?: number | string) {
  if (seconds === undefined || seconds === null || seconds === "") return "—";
  const value = Number(seconds);
  return Number.isFinite(value)
    ? new Date(value * 1000).toLocaleString("zh-CN", { hour12: false })
    : "—";
}

function CountCard({
  title,
  value,
  hint,
}: {
  title: string;
  value: string;
  hint: string;
}) {
  return (
    <Card className="rounded-lg shadow-sm">
      <CardHeader className="pb-2">
        <CardTitle className="text-xs font-medium uppercase tracking-wide text-muted-foreground">
          {title}
        </CardTitle>
      </CardHeader>
      <CardContent>
        <div className="text-2xl font-semibold tabular-nums">{value}</div>
        <div className="mt-1 text-xs text-muted-foreground">{hint}</div>
      </CardContent>
    </Card>
  );
}

export function ConfigApply({ onChanged }: { onChanged?: () => void }) {
  const [status, setStatus] = useState<ConfigStatus | null>(null);
  const [versions, setVersions] = useState<VersionRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [validation, setValidation] = useState<{
    valid: boolean;
    error?: string;
  } | null>(null);
  const [applyOpen, setApplyOpen] = useState(false);
  const [confirmText, setConfirmText] = useState("");
  const [rollbackVersion, setRollbackVersion] = useState<string | null>(null);

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [nextStatus, nextVersions] = await Promise.all([
        apiGet<ConfigStatus>("/config/status"),
        apiGet<VersionRow[]>("/config/versions"),
      ]);
      setStatus(nextStatus);
      setVersions(nextVersions);
      onChanged?.();
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : "加载配置状态失败");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function validate() {
    setBusy(true);
    try {
      const result = await apiPost<{ valid: boolean; error?: string }>(
        "/config/validate",
      );
      setValidation(result);
      if (result.valid) toast.success("校验通过，可以发布");
      else toast.error("校验失败", { description: result.error });
    } catch (validateError) {
      setValidation({
        valid: false,
        error: validateError instanceof Error ? validateError.message : "校验失败",
      });
    } finally {
      setBusy(false);
    }
  }

  async function apply() {
    setBusy(true);
    try {
      const result = await apiPost<ApplyResult>("/config/apply");
      setApplyOpen(false);
      setConfirmText("");
      setValidation(null);
      if (result.warning) toast.warning("运行时已生效，但版本记录失败", { description: result.warning });
      else toast.success(`配置已发布${result.version ? ` · v${result.version}` : ""}`);
      await load();
    } catch (applyError) {
      toast.error(applyError instanceof Error ? applyError.message : "发布失败");
    } finally {
      setBusy(false);
    }
  }

  async function rollback() {
    if (!rollbackVersion) return;
    setBusy(true);
    try {
      const result = await apiPost<ApplyResult>(
        `/config/versions/${encodeURIComponent(rollbackVersion)}/rollback`,
      );
      setRollbackVersion(null);
      setValidation(null);
      if (result.warning) toast.warning("运行时已回滚，但版本记录失败", { description: result.warning });
      else toast.success(`已回滚并生成新版本${result.version ? ` v${result.version}` : ""}`);
      await load();
    } catch (rollbackError) {
      toast.error(rollbackError instanceof Error ? rollbackError.message : "回滚失败");
    } finally {
      setBusy(false);
    }
  }

  const counts = status?.counts ?? {};
  const totalDraftObjects =
    (counts.transports ?? 0)
    + (counts.datapoints ?? 0)
    + (counts.poll_ranges ?? 0)
    + (counts.codecs ?? 0);

  return (
    <>
      <PageHeader
        title="配置发布"
        en="Config & Apply"
        description="检查数据库草稿、验证生成的 TOML，并以事务式热重载发布到运行时。"
        actions={
          <>
            <Button variant="ghost" size="sm" onClick={load} disabled={loading || busy}>
              <RefreshCw className="size-4" />刷新
            </Button>
            <Button variant="outline" size="sm" onClick={validate} disabled={loading || busy}>
              {busy ? <Loader2 className="size-4 animate-spin" /> : <ShieldCheck className="size-4" />}
              校验
            </Button>
            <Button
              size="sm"
              onClick={() => setApplyOpen(true)}
              disabled={loading || busy || !status?.draftDirty}
            >
              <UploadCloud className="size-4" />发布
            </Button>
          </>
        }
      />

      <main className="space-y-4 p-6">
        {loading ? (
          <div className="flex items-center justify-center gap-2 rounded-lg border bg-card py-20 text-sm text-muted-foreground">
            <Loader2 className="size-4 animate-spin" />正在读取配置状态…
          </div>
        ) : error ? (
          <div className="flex flex-col items-center gap-3 rounded-lg border border-dashed bg-card py-16 text-center">
            <XCircle className="size-7 text-destructive" />
            <div className="font-medium">配置状态加载失败</div>
            <div className="text-sm text-muted-foreground">{error}</div>
            <Button variant="outline" size="sm" onClick={load}>重试</Button>
          </div>
        ) : status ? (
          <>
            <section className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
              <CountCard
                title="当前生效版本"
                value={status.activeVersion ? `v${status.activeVersion}` : "未发布"}
                hint={status.activeVersion ? `${status.author || "未知用户"} · ${formatTime(status.appliedAt)}` : "首次发布将创建 v1"}
              />
              <CountCard
                title="草稿状态"
                value={status.draftDirty ? "待发布" : "已同步"}
                hint={`${totalDraftObjects} 个运行时配置对象`}
              />
              <CountCard
                title="运行时"
                value={status.runtimeRunning ? "运行中" : "已停止"}
                hint={status.runtimeRunning ? "可执行热重载" : "发布时将尝试启动"}
              />
              <CountCard
                title="配置规模"
                value={`${counts.datapoints ?? 0} 点`}
                hint={`${counts.transports ?? 0} 连接 · ${counts.poll_ranges ?? 0} 轮询`}
              />
            </section>

            {validation && (
              <div
                className={`flex items-start gap-2 rounded-lg border p-3 text-sm ${
                  validation.valid
                    ? "border-emerald-200 bg-emerald-50 text-emerald-700"
                    : "border-red-200 bg-red-50 text-red-700"
                }`}
              >
                {validation.valid
                  ? <CheckCircle2 className="mt-0.5 size-4 shrink-0" />
                  : <XCircle className="mt-0.5 size-4 shrink-0" />}
                <div>
                  <div className="font-medium">{validation.valid ? "校验通过" : "校验失败"}</div>
                  {validation.error && <div className="mt-1 text-xs">{validation.error}</div>}
                </div>
              </div>
            )}

            <section className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_minmax(520px,1.2fr)]">
              <Card className="overflow-hidden rounded-lg">
                <CardHeader className="border-b py-3">
                  <CardTitle className="flex items-center gap-2 text-sm">
                    <FileCode2 className="size-4 text-primary" />待发布 TOML
                  </CardTitle>
                </CardHeader>
                <CardContent className="p-0">
                  <pre className="max-h-[520px] overflow-auto bg-slate-950 p-4 font-mono text-xs leading-relaxed text-slate-100">
                    {status.renderedToml}
                  </pre>
                </CardContent>
              </Card>

              <Card className="overflow-hidden rounded-lg">
                <CardHeader className="border-b py-3">
                  <CardTitle className="flex items-center gap-2 text-sm">
                    <History className="size-4 text-primary" />版本历史
                  </CardTitle>
                </CardHeader>
                <CardContent className="p-0">
                  <Table>
                    <TableHeader>
                      <TableRow>
                        <TableHead>版本</TableHead>
                        <TableHead>状态</TableHead>
                        <TableHead>操作者</TableHead>
                        <TableHead>时间</TableHead>
                        <TableHead>说明</TableHead>
                        <TableHead className="text-right">操作</TableHead>
                      </TableRow>
                    </TableHeader>
                    <TableBody>
                      {versions.map((version) => (
                        <TableRow key={version.version}>
                          <TableCell className="font-mono font-medium">v{version.version}</TableCell>
                          <TableCell>
                            <Badge variant="outline">
                              {version.status === "active" ? "生效中" : "已归档"}
                            </Badge>
                          </TableCell>
                          <TableCell>{version.author || "—"}</TableCell>
                          <TableCell className="whitespace-nowrap text-xs text-muted-foreground">
                            {formatTime(version.applied_at)}
                          </TableCell>
                          <TableCell>{version.note || "—"}</TableCell>
                          <TableCell className="text-right">
                            <Button
                              variant="ghost"
                              size="sm"
                              disabled={version.status === "active" || busy}
                              onClick={() => setRollbackVersion(version.version)}
                            >
                              <RotateCcw className="size-3.5" />回滚
                            </Button>
                          </TableCell>
                        </TableRow>
                      ))}
                      {versions.length === 0 && (
                        <TableRow>
                          <TableCell colSpan={6} className="h-24 text-center text-muted-foreground">
                            暂无发布记录
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </CardContent>
              </Card>
            </section>
          </>
        ) : null}
      </main>

      <Dialog open={applyOpen} onOpenChange={setApplyOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>发布当前草稿配置？</DialogTitle>
            <DialogDescription>
              后端会重新校验并进行事务式热重载；失败时保留上一份运行配置。
            </DialogDescription>
          </DialogHeader>
          <Input
            value={confirmText}
            onChange={(event) => setConfirmText(event.target.value)}
            placeholder="输入 APPLY 确认"
            className="font-mono"
          />
          <DialogFooter>
            <Button variant="outline" onClick={() => setApplyOpen(false)}>取消</Button>
            <Button onClick={apply} disabled={confirmText !== "APPLY" || busy}>
              {busy && <Loader2 className="size-4 animate-spin" />}确认发布
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog
        open={!!rollbackVersion}
        onOpenChange={(open) => !open && setRollbackVersion(null)}
      >
        <DialogContent>
          <DialogHeader>
            <DialogTitle>回滚到 v{rollbackVersion}？</DialogTitle>
            <DialogDescription>
              该版本会立即加载到运行时并生成一条新的活动版本记录。数据库中的当前草稿不会被覆盖，因此回滚后仍可能显示“待发布”。
            </DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="outline" onClick={() => setRollbackVersion(null)}>取消</Button>
            <Button onClick={rollback} disabled={busy}>
              {busy ? <Loader2 className="size-4 animate-spin" /> : <RotateCcw className="size-4" />}
              确认回滚
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  );
}
