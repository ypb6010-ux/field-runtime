import type { ReactNode } from "react";
import { Lock } from "lucide-react";
import { Button } from "./ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "./ui/tooltip";
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from "./ui/alert-dialog";

interface PermissionButtonProps {
  /** 前端体验层：是否具备权限。后端仍是最终权威。 */
  allowed: boolean;
  /** 高风险操作：使用危险样式并要求二次确认 */
  danger?: boolean;
  confirm?: { title: string; description: string; confirmText?: string };
  deniedHint?: string;
  onAction: () => void;
  children: ReactNode;
  size?: "default" | "sm" | "lg";
  className?: string;
}

/**
 * 按钮级权限控制组件。
 * - 无权限：disabled + tooltip 提示（用 span 包裹以便 hover 触发）
 * - 高风险：危险样式 + 二次确认弹窗
 * 注意：菜单过滤与按钮禁用仅为体验，后端仍需校验权限。
 */
export function PermissionButton({
  allowed,
  danger,
  confirm,
  deniedHint = "无权限，请联系管理员",
  onAction,
  children,
  size = "default",
  className,
}: PermissionButtonProps) {
  if (!allowed) {
    return (
      <Tooltip>
        <TooltipTrigger asChild>
          {/* span 包裹：disabled 按钮自身不触发 hover 事件 */}
          <span className="inline-flex cursor-not-allowed">
            <Button variant={danger ? "destructive" : "default"} size={size} disabled className={className}>
              <Lock className="size-3.5" />
              {children}
            </Button>
          </span>
        </TooltipTrigger>
        <TooltipContent>{deniedHint}</TooltipContent>
      </Tooltip>
    );
  }

  if (danger && confirm) {
    return (
      <AlertDialog>
        <AlertDialogTrigger asChild>
          <Button variant="destructive" size={size} className={className}>
            {children}
          </Button>
        </AlertDialogTrigger>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>{confirm.title}</AlertDialogTitle>
            <AlertDialogDescription>{confirm.description}</AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>取消</AlertDialogCancel>
            <AlertDialogAction
              onClick={onAction}
              className="bg-destructive text-white hover:bg-destructive/90"
            >
              {confirm.confirmText ?? "确认执行"}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    );
  }

  return (
    <Button variant={danger ? "destructive" : "default"} size={size} className={className} onClick={onAction}>
      {children}
    </Button>
  );
}
