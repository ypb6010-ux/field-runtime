import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { AlertCircle, DownloadCloud, Loader2, Plus, RefreshCw } from "lucide-react";
import type { KindSchema, Transport, TransportPayload } from "../transports";
import {
  createTransport,
  deleteTransport,
  fetchKinds,
  fetchTransports,
  testConnection,
  updateTransport,
} from "../transports";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { EmptyState } from "../components/StateViews";
import { Button } from "../components/ui/button";
import { ProtocolFilterBar, type Filters } from "../components/protocols/ProtocolFilterBar";
import { TransportTable } from "../components/protocols/TransportTable";
import { TransportDrawer } from "../components/protocols/TransportDrawer";
import { TransportDetailDrawer } from "../components/protocols/TransportDetailDrawer";
import { DangerousConfirmModal } from "../components/protocols/DangerousConfirmModal";

const EMPTY_FILTERS: Filters = { keyword: "", kind: "all", status: "all" };

interface ConfirmState {
  open: boolean;
  kind: "delete" | "disable";
  target: Transport | null;
}

export function Protocols({
  canWrite,
  onDraftIncrement,
}: {
  canWrite: boolean;
  onDraftIncrement: () => void;
}) {
  const [rows, setRows] = useState<Transport[]>([]);
  const [filters, setFilters] = useState<Filters>(EMPTY_FILTERS);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  // kinds（来自 /transports/kinds）
  const [kinds, setKinds] = useState<KindSchema[]>([]);
  const [kindsLoading, setKindsLoading] = useState(false);
  const [kindsError, setKindsError] = useState<string | null>(null);

  // drawers
  const [drawer, setDrawer] = useState<{ open: boolean; mode: "create" | "edit"; initial: Transport | null }>({
    open: false,
    mode: "create",
    initial: null,
  });
  const [detail, setDetail] = useState<{ open: boolean; t: Transport | null }>({ open: false, t: null });
  const [confirm, setConfirm] = useState<ConfirmState>({ open: false, kind: "delete", target: null });

  async function loadKinds() {
    setKindsLoading(true);
    setKindsError(null);
    try {
      const k = await fetchKinds();
      setKinds(k);
    } catch (e) {
      setKindsError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setKindsLoading(false);
    }
  }

  useEffect(() => {
    load();
  }, []);

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const [list, k] = await Promise.all([fetchTransports(), fetchKinds()]);
      setRows(list);
      setKinds(k);
      setKindsError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : "加载失败");
    } finally {
      setLoading(false);
    }
  }

  const filtered = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return rows.filter((r) => {
      if (filters.kind !== "all" && r.kind !== filters.kind) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (kw && !`${r.id} ${r.name} ${r.endpoint}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const isFiltering =
    filters.keyword !== "" || filters.kind !== "all" || filters.status !== "all";

  function openCreate() {
    setDrawer({ open: true, mode: "create", initial: null });
  }
  function openEdit(t: Transport) {
    setDetail({ open: false, t: null });
    setDrawer({ open: true, mode: "edit", initial: t });
  }

  async function handleRowTest(t: Transport) {
    const id = toast.loading(`正在测试「${t.name}」…`);
    try {
      const r = await testConnection(t.kind, t.config, t.id);
      if (r.ok) {
        toast.success(`「${t.name}」连接正常${r.latencyMs !== undefined ? ` · ${r.latencyMs}ms` : ""}`, { id });
      } else {
        toast.error(`「${t.name}」连接失败${r.errorType ? ` · ${r.errorType}` : ""}`, { id, description: r.message });
      }
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "测试失败", { id });
    }
  }

  function handleToggle(t: Transport) {
    if (t.enabled) {
      setConfirm({ open: true, kind: "disable", target: t });
    } else {
      saveToggle(t, true);
    }
  }

  function handleDelete(t: Transport) {
    setConfirm({ open: true, kind: "delete", target: t });
  }

  async function saveToggle(t: Transport, enabled: boolean) {
    try {
      await updateTransport(t.id, {
        id: t.id,
        name: t.name,
        kind: t.kind,
        enabled,
        params_json: t.config,
        scheduler_json: t.scheduler,
      });
      onDraftIncrement();
      toast.success(enabled ? `已启用「${t.name}」，存在未生效配置，请到 Config & Apply 发布` : `已停用「${t.name}」，其关联点位将停止采集`);
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "保存失败");
    }
  }

  async function confirmAction() {
    const t = confirm.target;
    if (!t) return;
    try {
      if (confirm.kind === "delete") {
        await deleteTransport(t.id);
      } else {
        await updateTransport(t.id, {
          id: t.id,
          name: t.name,
          kind: t.kind,
          enabled: false,
          params_json: t.config,
          scheduler_json: t.scheduler,
        });
      }
      onDraftIncrement();
      setConfirm({ open: false, kind: "delete", target: null });
      toast.success(confirm.kind === "delete" ? `已删除「${t.name}」，存在未生效配置，请到 Config & Apply 发布` : `已停用「${t.name}」，其关联点位将停止采集`);
      load();
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "操作失败");
    }
  }

  async function handleSave(
    mode: "create" | "edit",
    payload: TransportPayload,
  ) {
    if (mode === "create") await createTransport(payload);
    else await updateTransport(payload.id, payload);
    await load();
    await onDraftIncrement();
    toast.success("已保存为草稿，需要到 Config & Apply 发布后生效");
  }

  return (
    <>
      <PageHeader
        title="协议管理"
        en="Protocols"
        description="管理工业协议连接，配置表单由后端 Schema 动态生成。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={load}>
              <RefreshCw className="size-3.5" />
              刷新
            </Button>
            <Button
              variant="outline"
              size="sm"
              className="gap-1.5"
              disabled={kindsLoading}
              onClick={() => loadKinds()}
            >
              <DownloadCloud className="size-3.5" />
              同步协议类型
            </Button>
            <PermissionButton allowed={canWrite} onAction={openCreate} size="sm">
              <Plus className="size-3.5" />
              新增协议
            </PermissionButton>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <ProtocolFilterBar
          filters={filters}
          kinds={kinds}
          onChange={setFilters}
          onReset={() => setFilters(EMPTY_FILTERS)}
        />

        <div className="flex items-center justify-between text-xs text-muted-foreground">
          <span>
            共 {filtered.length} 条连接{isFiltering ? "（已筛选）" : ""}
          </span>
        </div>

        {loading ? (
          <div className="flex items-center justify-center gap-2 rounded-md border border-border bg-card py-20 text-muted-foreground">
            <Loader2 className="size-4 animate-spin" />加载中…
          </div>
        ) : error ? (
          <div className="flex flex-col items-center gap-3 rounded-md border border-dashed border-border bg-card py-16 text-center">
            <AlertCircle className="size-7 text-red-500" />
            <div className="text-sm font-medium">加载失败</div>
            <div className="text-sm text-muted-foreground">{error}</div>
            <Button variant="outline" size="sm" onClick={load}>重试</Button>
          </div>
        ) : filtered.length === 0 ? (
          <div className="rounded-md border border-border bg-card">
            <EmptyState
              title={isFiltering ? "没有匹配的协议连接" : "暂无协议连接"}
              description={isFiltering ? "调整筛选条件，或清空后重试。" : "还没有任何协议连接，先新增一个。"}
              action={
                isFiltering ? (
                  <Button variant="outline" size="sm" onClick={() => setFilters(EMPTY_FILTERS)}>
                    清空筛选
                  </Button>
                ) : (
                  <PermissionButton allowed={canWrite} onAction={openCreate} size="sm">
                    <Plus className="size-3.5" />
                    新增协议
                  </PermissionButton>
                )
              }
            />
          </div>
        ) : (
          <TransportTable
            rows={filtered}
            canWrite={canWrite}
            onRowClick={(t) => setDetail({ open: true, t })}
            onEdit={openEdit}
            onTest={handleRowTest}
            onToggle={handleToggle}
            onDelete={handleDelete}
          />
        )}
      </div>

      {/* 新增 / 编辑 Drawer */}
      <TransportDrawer
        open={drawer.open}
        onOpenChange={(o) => setDrawer((d) => ({ ...d, open: o }))}
        mode={drawer.mode}
        initial={drawer.initial}
        kinds={kinds}
        kindsLoading={kindsLoading}
        kindsError={kindsError}
        onRetryKinds={() => loadKinds()}
        onSave={handleSave}
      />

      {/* 详情 Drawer */}
      <TransportDetailDrawer
        transport={detail.t}
        open={detail.open}
        onOpenChange={(o) => setDetail((d) => ({ ...d, open: o }))}
        canWrite={canWrite}
        onEdit={openEdit}
      />

      {/* 删除 / 停用二次确认 */}
      <DangerousConfirmModal
        open={confirm.open}
        onOpenChange={(o) => setConfirm((c) => ({ ...c, open: o }))}
        title={confirm.kind === "delete" ? "确认删除该协议连接？" : "确认停用该协议连接？"}
        description={
          confirm.kind === "delete" ? (
            <>
              将删除「{confirm.target?.name}」及其配置。该操作不可恢复，关联的 {confirm.target?.pointCount}{" "}
              个关联采集点和轮询任务也会一并删除；若仍被转换规则引用，后端会拒绝删除。
            </>
          ) : (
            <>
              停用「{confirm.target?.name}」后，其关联的 {confirm.target?.pointCount}{" "}
              个采集点将停止采集，历史数据保留。
            </>
          )
        }
        confirmText={confirm.kind === "delete" ? "确认删除" : "确认停用"}
        onConfirm={confirmAction}
      />
    </>
  );
}
