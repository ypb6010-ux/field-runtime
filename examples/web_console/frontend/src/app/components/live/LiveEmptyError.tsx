import { ServerCrash, RefreshCw, ExternalLink, Inbox, Plus } from "lucide-react";
import { Button } from "../ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";

/** GET /data/latest 加载失败 — 完整错误卡片，含技术详情 */
export function LatestDataErrorState({ onRetry }: { onRetry: () => void }) {
  return (
    <div className="space-y-3 rounded-md border border-status-error-border bg-card p-5">
      {/* 标题 */}
      <div className="flex items-start gap-3">
        <div className="flex size-10 shrink-0 items-center justify-center rounded-md bg-status-error-bg">
          <ServerCrash className="size-5 text-status-error" />
        </div>
        <div>
          <div className="text-sm text-status-error">最新数据加载失败</div>
          <p className="mt-0.5 text-xs text-muted-foreground">
            无法从 <span className="font-mono">GET /data/latest</span> 获取点位最新数据，
            请检查后端服务或网络连接。WebSocket 订阅将在恢复后继续工作。
          </p>
        </div>
      </div>

      {/* 技术详情 */}
      <div className="rounded-md border border-status-error-border/40 bg-status-error-bg/40 px-3 py-2.5">
        <p className="mb-1.5 text-[11px] text-muted-foreground">错误详情</p>
        <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 font-mono text-[11px]">
          <dt className="text-muted-foreground">request</dt>
          <dd>GET /data/latest</dd>
          <dt className="text-muted-foreground">status</dt>
          <dd className="text-status-error">503 Service Unavailable</dd>
          <dt className="text-muted-foreground">message</dt>
          <dd className="text-foreground/70">data service unavailable</dd>
        </dl>
      </div>

      {/* 操作 */}
      <div className="flex items-center gap-2">
        <Button variant="outline" size="sm" onClick={onRetry} className="gap-1.5">
          <RefreshCw className="size-3.5" />
          重试
        </Button>
        <Button variant="link" size="sm" className="h-auto gap-1 p-0 text-xs">
          <ExternalLink className="size-3" />
          查看系统健康状态
        </Button>
      </div>
    </div>
  );
}

/** 没有匹配点位的空态 */
export function LiveEmptyState({
  isFiltering,
  canConfig,
  onReset,
}: {
  isFiltering: boolean;
  canConfig: boolean;
  onReset: () => void;
}) {
  return (
    <div className="flex flex-col items-center justify-center gap-4 rounded-md border border-border bg-card py-16 text-center">
      <div className="flex size-12 items-center justify-center rounded-full bg-muted">
        <Inbox className="size-6 text-muted-foreground" />
      </div>
      <div>
        <div className="text-sm">{isFiltering ? "没有匹配的点位" : "暂无实时点位"}</div>
        <p className="mt-1 max-w-sm text-xs text-muted-foreground">
          {isFiltering
            ? "当前筛选条件下没有匹配的点位，请调整筛选条件或先配置采集点。"
            : "请调整筛选条件或先配置采集点。"}
        </p>
      </div>
      <div className="flex gap-2">
        {isFiltering && (
          <Button variant="outline" size="sm" onClick={onReset}>
            重置筛选
          </Button>
        )}
        {canConfig ? (
          <Button variant="outline" size="sm" className="gap-1.5">
            <Plus className="size-3.5" />
            去采集点配置
          </Button>
        ) : (
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="inline-flex">
                <Button variant="outline" size="sm" disabled className="gap-1.5 cursor-not-allowed">
                  <Plus className="size-3.5" />
                  去采集点配置
                </Button>
              </span>
            </TooltipTrigger>
            <TooltipContent>无配置权限，请联系管理员</TooltipContent>
          </Tooltip>
        )}
      </div>
    </div>
  );
}
