import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { Plus, RefreshCw, DownloadCloud } from "lucide-react";
import type { KindSchema, Transport } from "../transports";
import {
  SEED_TRANSPORTS,
  ALL_TAGS,
  fetchKinds,
  setKindsFailMode,
  testConnection,
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

const EMPTY_FILTERS: Filters = { keyword: "", kind: "all", status: "all", tag: "all" };

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
  const [rows, setRows] = useState<Transport[]>(SEED_TRANSPORTS);
  const [filters, setFilters] = useState<Filters>(EMPTY_FILTERS);

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

  async function loadKinds(fail = false) {
    setKindsLoading(true);
    setKindsError(null);
    setKindsFailMode(fail);
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
    loadKinds();
  }, []);

  const filtered = useMemo(() => {
    const kw = filters.keyword.trim().toLowerCase();
    return rows.filter((r) => {
      if (filters.kind !== "all" && r.kind !== filters.kind) return false;
      if (filters.status !== "all" && r.status !== filters.status) return false;
      if (filters.tag !== "all" && !r.tags.includes(filters.tag)) return false;
      if (kw && !`${r.name} ${r.endpoint}`.toLowerCase().includes(kw)) return false;
      return true;
    });
  }, [rows, filters]);

  const isFiltering =
    filters.keyword !== "" || filters.kind !== "all" || filters.status !== "all" || filters.tag !== "all";

  function openCreate() {
    setDrawer({ open: true, mode: "create", initial: null });
  }
  function openEdit(t: Transport) {
    setDetail({ open: false, t: null });
    setDrawer({ open: true, mode: "edit", initial: t });
  }

  async function handleRowTest(t: Transport) {
    const id = toast.loading(`正在测试「${t.name}」…`);
    const r = await testConnection(t.kind, t.config);
    if (r.ok) {
      toast.success(`「${t.name}」连接正常 · ${r.latencyMs}ms`, { id });
    } else {
      toast.error(`「${t.name}」连接失败 · ${r.errorType}`, { id, description: r.message });
    }
  }

  function handleToggle(t: Transport) {
    if (t.enabled) {
      setConfirm({ open: true, kind: "disable", target: t });
    } else {
      setRows((p) => p.map((x) => (x.id === t.id ? { ...x, enabled: true, status: "online" } : x)));
      onDraftIncrement();
      toast.success(`已启用「${t.name}」，存在未生效配置，请到 Config & Apply 发布`);
    }
  }

  function handleDelete(t: Transport) {
    setConfirm({ open: true, kind: "delete", target: t });
  }

  function confirmAction() {
    const t = confirm.target;
    if (!t) return;
    if (confirm.kind === "delete") {
      setRows((p) => p.filter((x) => x.id !== t.id));
      toast.success(`已删除「${t.name}」，存在未生效配置，请到 Config & Apply 发布`);
    } else {
      setRows((p) =>
        p.map((x) => (x.id === t.id ? { ...x, enabled: false, status: "disabled" } : x)),
      );
      toast.warning(`已停用「${t.name}」，其关联点位将停止采集`);
    }
    onDraftIncrement();
    setConfirm({ open: false, kind: "delete", target: null });
  }

  function handleSaved(mode: "create" | "edit") {
    onDraftIncrement();
    toast.success("已保存为草稿，需要到 Config & Apply 发布后生效", {
      description: mode === "create" ? "新增的协议连接已进入未生效配置" : "修改已记录为 draft config diff",
    });
  }

  return (
    <>
      <PageHeader
        title="协议管理"
        en="Protocols"
        description="管理工业协议连接，配置表单由后端 Schema 动态生成。"
        actions={
          <>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => toast.message("已刷新列表")}>
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
          tags={ALL_TAGS}
          onChange={setFilters}
          onReset={() => setFilters(EMPTY_FILTERS)}
        />

        {/* 演示：模拟 /transports/kinds 加载失败（仅骨架阶段） */}
        <div className="flex items-center justify-between text-xs text-muted-foreground">
          <span>
            共 {filtered.length} 条连接{isFiltering ? "（已筛选）" : ""}
          </span>
          <button
            type="button"
            className="text-muted-foreground/70 underline-offset-2 hover:underline"
            onClick={() => {
              loadKinds(true);
              toast.message("已模拟 kinds 加载失败，打开「新增协议」可见错误态");
            }}
          >
            演示：模拟 kinds 加载失败
          </button>
        </div>

        {filtered.length === 0 ? (
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
        onSaved={handleSaved}
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
              个采集点将失去数据源。
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
