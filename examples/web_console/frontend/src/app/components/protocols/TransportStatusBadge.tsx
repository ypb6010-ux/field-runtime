import type { TransportStatus } from "../../transports";
import { STATUS_META } from "../../transports";
import { Badge } from "../ui/badge";

const TONE_CLASS: Record<string, string> = {
  success: "border-status-success-border bg-status-success-bg text-status-success",
  warning: "border-status-warning-border bg-status-warning-bg text-status-warning",
  error: "border-status-error-border bg-status-error-bg text-status-error",
  disabled: "border-border bg-muted text-status-disabled",
};

/** 协议连接状态徽标：在线/离线/重连中/错误/禁用 */
export function TransportStatusBadge({ status }: { status: TransportStatus }) {
  const meta = STATUS_META[status];
  const dot =
    meta.tone === "success"
      ? "bg-status-success"
      : meta.tone === "warning"
        ? "bg-status-warning"
        : meta.tone === "error"
          ? "bg-status-error"
          : "bg-status-disabled";
  return (
    <Badge variant="outline" className={`gap-1.5 ${TONE_CLASS[meta.tone]}`}>
      <span
        className={`size-1.5 rounded-full ${dot} ${status === "reconnecting" ? "animate-pulse" : ""}`}
      />
      {meta.label}
    </Badge>
  );
}
