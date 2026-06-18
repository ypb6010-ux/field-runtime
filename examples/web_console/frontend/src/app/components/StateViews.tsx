import type { ReactNode } from "react";
import {
  Loader2,
  Inbox,
  AlertTriangle,
  ShieldX,
  WifiOff,
  RefreshCw,
  PencilLine,
  GitCompareArrows,
} from "lucide-react";
import { Button } from "./ui/button";
import { cn } from "./ui/utils";

/** 通用状态块：居中图标 + 标题 + 描述 + 操作。用于占满列表/卡片空间。 */
function StateBlock({
  icon,
  title,
  description,
  action,
  iconClass,
  className,
}: {
  icon: ReactNode;
  title: string;
  description?: string;
  action?: ReactNode;
  iconClass?: string;
  className?: string;
}) {
  return (
    <div className={cn("flex flex-col items-center justify-center gap-3 px-6 py-10 text-center", className)}>
      <div className={cn("flex size-12 items-center justify-center rounded-full bg-muted", iconClass)}>
        {icon}
      </div>
      <div>
        <div className="text-sm">{title}</div>
        {description && <p className="mt-1 max-w-xs text-xs text-muted-foreground">{description}</p>}
      </div>
      {action}
    </div>
  );
}

export function LoadingState({ label = "加载中…" }: { label?: string }) {
  return (
    <StateBlock
      icon={<Loader2 className="size-6 animate-spin text-primary" />}
      iconClass="bg-secondary"
      title={label}
      description="正在请求数据，请稍候。"
    />
  );
}

export function EmptyState({
  title = "暂无数据",
  description = "当前没有可显示的记录。",
  action,
}: {
  title?: string;
  description?: string;
  action?: ReactNode;
}) {
  return (
    <StateBlock
      icon={<Inbox className="size-6 text-muted-foreground" />}
      title={title}
      description={description}
      action={action}
    />
  );
}

export function ErrorState({
  title = "加载失败",
  description = "数据请求出错，请重试。",
  onRetry,
}: {
  title?: string;
  description?: string;
  onRetry?: () => void;
}) {
  return (
    <StateBlock
      icon={<AlertTriangle className="size-6 text-status-error" />}
      iconClass="bg-status-error-bg"
      title={title}
      description={description}
      action={
        onRetry && (
          <Button variant="outline" size="sm" onClick={onRetry} className="gap-1.5">
            <RefreshCw className="size-3.5" />
            重试
          </Button>
        )
      }
    />
  );
}

export function PermissionDenied({
  description = "你没有访问该资源的权限，请联系管理员。",
  onRefresh,
}: {
  description?: string;
  onRefresh?: () => void;
}) {
  return (
    <StateBlock
      icon={<ShieldX className="size-6 text-status-disabled" />}
      iconClass="bg-status-disabled-bg"
      title="无访问权限"
      description={description}
      action={
        onRefresh && (
          <Button variant="outline" size="sm" onClick={onRefresh} className="gap-1.5">
            <RefreshCw className="size-3.5" />
            刷新权限
          </Button>
        )
      }
    />
  );
}

export function OfflineState({ onRetry }: { onRetry?: () => void }) {
  return (
    <StateBlock
      icon={<WifiOff className="size-6 text-status-error" />}
      iconClass="bg-status-error-bg"
      title="网络已断开"
      description="无法连接服务器，请检查网络连接。"
      action={
        onRetry && (
          <Button variant="outline" size="sm" onClick={onRetry} className="gap-1.5">
            <RefreshCw className="size-3.5" />
            重新连接
          </Button>
        )
      }
    />
  );
}

export function ReconnectingState({ label = "正在重连…" }: { label?: string }) {
  return (
    <StateBlock
      icon={<RefreshCw className="size-6 animate-spin text-status-warning" />}
      iconClass="bg-status-warning-bg"
      title={label}
      description="实时通道中断，正在尝试恢复连接。"
    />
  );
}

/** 内联提示条：未保存的更改 */
export function UnsavedChangesBar({
  onSave,
  onDiscard,
}: {
  onSave?: () => void;
  onDiscard?: () => void;
}) {
  return (
    <div className="flex items-center justify-between gap-3 rounded-md border border-status-warning-border bg-status-warning-bg px-4 py-2.5">
      <span className="flex items-center gap-2 text-sm text-status-warning">
        <PencilLine className="size-4" />
        有未保存的更改
      </span>
      <div className="flex gap-2">
        <Button variant="ghost" size="sm" onClick={onDiscard}>
          放弃
        </Button>
        <Button size="sm" onClick={onSave}>
          保存
        </Button>
      </div>
    </div>
  );
}

/** 内联提示条：草稿与生效配置存在差异 */
export function DraftDiffBar({
  count = 3,
  onReview,
}: {
  count?: number;
  onReview?: () => void;
}) {
  return (
    <div className="flex items-center justify-between gap-3 rounded-md border border-status-draft-border bg-status-draft-bg px-4 py-2.5">
      <span className="flex items-center gap-2 text-sm text-status-draft">
        <GitCompareArrows className="size-4" />
        草稿配置与生效配置存在 {count} 项差异
      </span>
      <Button
        size="sm"
        className="bg-status-draft text-white hover:bg-status-draft/90"
        onClick={onReview}
      >
        查看并发布
      </Button>
    </div>
  );
}
