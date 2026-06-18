import { ServerCrash, RefreshCw, ExternalLink } from "lucide-react";
import { Button } from "../ui/button";

interface SchemaLoadErrorProps {
  message?: string;
  onRetry: () => void;
  retrying?: boolean;
  /** 接口技术详情（可选，便于高级用户排查） */
  detail?: {
    request?: string;
    status?: string;
    message?: string;
  };
}

/** GET /transports/kinds 失败时，替换 Kind 选择与动态表单区域 */
export function SchemaLoadErrorState({
  message,
  onRetry,
  retrying,
  detail,
}: SchemaLoadErrorProps) {
  const technicalDetail = detail ?? {
    request: "GET /transports/kinds",
    status: "503 Service Unavailable",
    message: message ?? "schema registry unavailable",
  };

  return (
    <div className="space-y-3 rounded-md border border-dashed border-status-error-border bg-status-error-bg p-4">
      {/* 标题 + 图标 */}
      <div className="flex items-start gap-3">
        <div className="flex size-9 shrink-0 items-center justify-center rounded-md bg-card">
          <ServerCrash className="size-5 text-status-error" />
        </div>
        <div className="min-w-0 flex-1">
          <div className="text-sm text-status-error">协议类型加载失败</div>
          <p className="mt-0.5 text-xs text-muted-foreground">
            无法从 <span className="font-mono">/transports/kinds</span> 获取协议 Schema，当前无法选择协议类型或生成动态表单。
          </p>
        </div>
      </div>

      {/* 技术详情 */}
      <div className="rounded-md border border-status-error-border/50 bg-card/60 px-3 py-2.5">
        <p className="mb-1.5 text-[11px] text-muted-foreground">错误详情</p>
        <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 font-mono text-[11px]">
          <dt className="text-muted-foreground">request</dt>
          <dd className="text-foreground/80">{technicalDetail.request}</dd>
          <dt className="text-muted-foreground">status</dt>
          <dd className="text-status-error">{technicalDetail.status}</dd>
          <dt className="text-muted-foreground">message</dt>
          <dd className="break-all text-foreground/70">{technicalDetail.message}</dd>
        </dl>
      </div>

      {/* 操作按钮 */}
      <div className="flex items-center gap-2">
        <Button variant="outline" size="sm" onClick={onRetry} disabled={retrying} className="gap-1.5">
          <RefreshCw className={`size-3.5 ${retrying ? "animate-spin" : ""}`} />
          重试
        </Button>
        <Button
          variant="link"
          size="sm"
          className="h-auto gap-1 p-0 text-xs"
          onClick={() => window.open("/status", "_blank")}
        >
          <ExternalLink className="size-3" />
          查看接口状态
        </Button>
      </div>
    </div>
  );
}
