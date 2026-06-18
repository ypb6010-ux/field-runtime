import type { ReactNode } from "react";
import { Pencil, PlugZap, Power, PowerOff, Trash2 } from "lucide-react";
import type { Transport } from "../../transports";
import { kindLabel } from "../../transports";
import { TransportStatusBadge } from "./TransportStatusBadge";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "../ui/table";
import { Badge } from "../ui/badge";
import { Button } from "../ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";

function IconAction({
  label,
  icon,
  onClick,
  disabled,
  deniedHint = "无权限，请联系管理员",
  danger,
}: {
  label: string;
  icon: ReactNode;
  onClick: () => void;
  disabled?: boolean;
  deniedHint?: string;
  danger?: boolean;
}) {
  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <span className="inline-flex">
          <Button
            variant="ghost"
            size="icon"
            className={`size-7 ${danger ? "text-status-error hover:text-status-error" : ""}`}
            disabled={disabled}
            onClick={(e) => {
              e.stopPropagation();
              onClick();
            }}
          >
            {icon}
          </Button>
        </span>
      </TooltipTrigger>
      <TooltipContent>{disabled ? deniedHint : label}</TooltipContent>
    </Tooltip>
  );
}

export function TransportTable({
  rows,
  canWrite,
  onRowClick,
  onEdit,
  onTest,
  onToggle,
  onDelete,
}: {
  rows: Transport[];
  canWrite: boolean;
  onRowClick: (t: Transport) => void;
  onEdit: (t: Transport) => void;
  onTest: (t: Transport) => void;
  onToggle: (t: Transport) => void;
  onDelete: (t: Transport) => void;
}) {
  return (
    <div className="overflow-x-auto rounded-md border border-border bg-card">
      <Table>
        <TableHeader>
          <TableRow className="bg-muted/50">
            <TableHead className="min-w-44">协议名称</TableHead>
            <TableHead>Kind</TableHead>
            <TableHead className="min-w-48">Endpoint</TableHead>
            <TableHead>状态</TableHead>
            <TableHead>启用</TableHead>
            <TableHead className="min-w-40">最近连接</TableHead>
            <TableHead className="min-w-48">最近错误</TableHead>
            <TableHead className="text-right">点位</TableHead>
            <TableHead>更新人</TableHead>
            <TableHead className="text-right">操作</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {rows.map((t) => (
            <TableRow key={t.id} className="cursor-pointer" onClick={() => onRowClick(t)}>
              <TableCell>
                <div className="flex flex-col">
                  <span>{t.name}</span>
                  {t.tags.length > 0 && (
                    <span className="flex flex-wrap gap-1 pt-0.5">
                      {t.tags.map((tag) => (
                        <span key={tag} className="rounded bg-muted px-1.5 py-0.5 text-[11px] text-muted-foreground">
                          {tag}
                        </span>
                      ))}
                    </span>
                  )}
                </div>
              </TableCell>
              <TableCell>
                <Badge variant="outline">{kindLabel(t.kind)}</Badge>
              </TableCell>
              <TableCell className="font-mono text-xs text-muted-foreground">{t.endpoint}</TableCell>
              <TableCell>
                <TransportStatusBadge status={t.status} />
              </TableCell>
              <TableCell>
                {t.enabled ? (
                  <span className="text-xs text-status-success">启用</span>
                ) : (
                  <span className="text-xs text-status-disabled">停用</span>
                )}
              </TableCell>
              <TableCell className="text-xs text-muted-foreground">{t.lastConnectedAt ?? "—"}</TableCell>
              <TableCell className="max-w-56">
                {t.lastError ? (
                  <Tooltip>
                    <TooltipTrigger asChild>
                      <span className="block truncate text-xs text-status-error">{t.lastError}</span>
                    </TooltipTrigger>
                    <TooltipContent className="max-w-xs">{t.lastError}</TooltipContent>
                  </Tooltip>
                ) : (
                  <span className="text-xs text-muted-foreground">—</span>
                )}
              </TableCell>
              <TableCell className="text-right tabular-nums">{t.pointCount}</TableCell>
              <TableCell className="text-xs text-muted-foreground">{t.updatedBy}</TableCell>
              <TableCell>
                <div className="flex items-center justify-end gap-0.5">
                  <IconAction label="编辑" icon={<Pencil className="size-3.5" />} disabled={!canWrite} onClick={() => onEdit(t)} />
                  <IconAction label="测试连接" icon={<PlugZap className="size-3.5" />} disabled={!canWrite} onClick={() => onTest(t)} />
                  <IconAction
                    label={t.enabled ? "停用" : "启用"}
                    icon={t.enabled ? <PowerOff className="size-3.5" /> : <Power className="size-3.5" />}
                    disabled={!canWrite}
                    onClick={() => onToggle(t)}
                  />
                  <IconAction label="删除" icon={<Trash2 className="size-3.5" />} disabled={!canWrite} danger onClick={() => onDelete(t)} />
                </div>
              </TableCell>
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </div>
  );
}
