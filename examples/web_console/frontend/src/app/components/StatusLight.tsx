import type { StatusTone } from "../types";
import { cn } from "./ui/utils";

const TONE: Record<StatusTone, { dot: string; text: string; ring: string }> = {
  success: { dot: "bg-status-success", text: "text-status-success", ring: "bg-status-success/30" },
  warning: { dot: "bg-status-warning", text: "text-status-warning", ring: "bg-status-warning/30" },
  error: { dot: "bg-status-error", text: "text-status-error", ring: "bg-status-error/30" },
  draft: { dot: "bg-status-draft", text: "text-status-draft", ring: "bg-status-draft/30" },
  disabled: { dot: "bg-status-disabled", text: "text-status-disabled", ring: "bg-status-disabled/20" },
  info: { dot: "bg-primary", text: "text-primary", ring: "bg-primary/30" },
};

interface StatusLightProps {
  tone: StatusTone;
  label?: string;
  /** 是否呼吸闪烁（重连中等动态状态） */
  pulse?: boolean;
  className?: string;
}

/** 状态指示灯：一个圆点 + 可选文字，可呼吸闪烁 */
export function StatusLight({ tone, label, pulse, className }: StatusLightProps) {
  const t = TONE[tone];
  return (
    <span className={cn("inline-flex items-center gap-1.5", className)}>
      <span className="relative inline-flex size-2.5 items-center justify-center">
        {pulse && (
          <span className={cn("absolute inline-flex size-2.5 animate-ping rounded-full", t.ring)} />
        )}
        <span className={cn("relative inline-flex size-2 rounded-full", t.dot)} />
      </span>
      {label && <span className={cn("text-xs", t.text)}>{label}</span>}
    </span>
  );
}
