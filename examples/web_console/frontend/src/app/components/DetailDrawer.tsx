import type { ReactNode } from "react";
import {
  Sheet,
  SheetContent,
  SheetHeader,
  SheetTitle,
  SheetDescription,
  SheetFooter,
} from "./ui/sheet";

interface DetailDrawerProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: ReactNode;
  description?: ReactNode;
  children?: ReactNode;
  footer?: ReactNode;
  width?: number;
}

/**
 * 右侧详情 / 编辑抽屉的统一模式。
 * 列表行点击 → 在不离开当前列表上下文的情况下查看 / 编辑详情。
 */
export function DetailDrawer({
  open,
  onOpenChange,
  title,
  description,
  children,
  footer,
  width = 480,
}: DetailDrawerProps) {
  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent
        side="right"
        className="flex flex-col gap-0 p-0 sm:max-w-none"
        style={{ width }}
      >
        <SheetHeader className="border-b border-border px-5 py-4">
          <SheetTitle>{title}</SheetTitle>
          {description && <SheetDescription>{description}</SheetDescription>}
        </SheetHeader>
        <div className="flex-1 overflow-y-auto px-5 py-4">{children}</div>
        {footer && (
          <SheetFooter className="flex-row justify-end gap-2 border-t border-border px-5 py-3">
            {footer}
          </SheetFooter>
        )}
      </SheetContent>
    </Sheet>
  );
}
