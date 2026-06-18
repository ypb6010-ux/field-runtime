import type { ReactNode } from "react";
import { toast } from "sonner";
import { PageHeader } from "../components/PageHeader";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import {
  LoadingState,
  EmptyState,
  ErrorState,
  PermissionDenied,
  OfflineState,
  ReconnectingState,
  UnsavedChangesBar,
  DraftDiffBar,
} from "../components/StateViews";
import { Button } from "../components/ui/button";

function Slot({ name, children }: { name: string; children: ReactNode }) {
  return (
    <Card className="gap-0 overflow-hidden">
      <div className="border-b border-border bg-muted/40 px-4 py-2">
        <CardTitle className="text-sm">{name}</CardTitle>
      </div>
      <CardContent className="p-0">{children}</CardContent>
    </Card>
  );
}

export function GlobalStates() {
  return (
    <>
      <PageHeader
        title="全局状态组件"
        en="Global State Components"
        description="可复用于所有业务页面的状态视图（设计说明，非业务页面）。"
      />
      <div className="space-y-6 p-6">
        {/* 占满区域的状态视图 */}
        <div className="grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-3">
          <Slot name="Loading">
            <LoadingState />
          </Slot>
          <Slot name="Empty">
            <EmptyState action={<Button size="sm">新建采集点</Button>} />
          </Slot>
          <Slot name="Error">
            <ErrorState onRetry={() => toast.message("重新请求…")} />
          </Slot>
          <Slot name="Permission Denied">
            <PermissionDenied onRefresh={() => toast.message("刷新 /auth/me …")} />
          </Slot>
          <Slot name="Offline">
            <OfflineState onRetry={() => toast.message("重新连接…")} />
          </Slot>
          <Slot name="Reconnecting">
            <ReconnectingState />
          </Slot>
        </div>

        {/* 内联提示条 */}
        <div className="space-y-3">
          <CardTitle className="text-sm">内联提示条 Inline Banners</CardTitle>
          <UnsavedChangesBar
            onSave={() => toast.success("已保存")}
            onDiscard={() => toast.message("已放弃更改")}
          />
          <DraftDiffBar count={3} onReview={() => toast.message("跳转 Config & Apply")} />
        </div>
      </div>
    </>
  );
}
