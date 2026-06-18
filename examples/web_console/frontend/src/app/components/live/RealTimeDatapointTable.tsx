import type { Datapoint, WsState } from "../../live";
import { STALE_THRESHOLD_S } from "../../live";
import { DatapointValueCell, QualityBadge, DpStatusBadge, StaleDataTag } from "./atoms";
import { ReadNowButton } from "./ReadNowButton";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "../ui/table";
import { Button } from "../ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";
import { LineChart, Info } from "lucide-react";
import { cn } from "../ui/utils";

interface RealTimeDatapointTableProps {
  rows: Datapoint[];
  canControl: boolean;
  wsState: WsState;
  /** 订阅失败的点位 ID 集合，显示订阅失败 Tag */
  failedSubIds?: Set<string>;
  selectedId?: string;
  onRowClick: (dp: Datapoint) => void;
  onTrend: (dp: Datapoint) => void;
  onReadNow: (dp: Datapoint, result: import("../../live").ReadResult) => void;
}

export function RealTimeDatapointTable({
  rows, canControl, wsState, failedSubIds, selectedId, onRowClick, onTrend, onReadNow,
}: RealTimeDatapointTableProps) {
  const wsDisconnected = wsState === "disconnected";
  return (
    <div className="overflow-x-auto rounded-md border border-border bg-card">
      <Table>
        <TableHeader>
          <TableRow className="bg-muted/50">
            <TableHead className="min-w-40">点位名称</TableHead>
            <TableHead className="min-w-36">当前值</TableHead>
            <TableHead>质量</TableHead>
            <TableHead>状态</TableHead>
            <TableHead className="min-w-36">时间戳</TableHead>
            <TableHead className="min-w-36">来源 Transport</TableHead>
            <TableHead>地址</TableHead>
            <TableHead className="text-right">操作</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {rows.map((dp) => {
            // WS 断开时所有行强制视为 stale（显示最后数据但降权）
            const isStale = wsDisconnected || dp.ageSeconds > STALE_THRESHOLD_S;
            const isSubFailed = failedSubIds?.has(dp.id) ?? false;
            const selected = dp.id === selectedId;
            return (
              <TableRow
                key={dp.id}
                className={cn(
                  "cursor-pointer transition-colors",
                  isStale && !selected && "opacity-60 bg-muted/20",
                  wsDisconnected && "grayscale-[0.3]",
                  selected && "bg-secondary/60 opacity-100",
                )}
                onClick={() => onRowClick(dp)}
              >
                <TableCell>
                  <div className="flex flex-col gap-0.5">
                    <span className="text-sm">{dp.name}</span>
                    <span className="flex flex-wrap gap-1">
                      {dp.tags.map((t) => (
                        <span key={t} className="rounded bg-muted px-1.5 py-0.5 text-[10px] text-muted-foreground">
                          {t}
                        </span>
                      ))}
                      {isSubFailed && (
                        <span className="rounded border border-status-warning-border bg-status-warning-bg px-1.5 py-0.5 text-[10px] text-status-warning">
                          订阅失败
                        </span>
                      )}
                    </span>
                  </div>
                </TableCell>
                <TableCell>
                  <DatapointValueCell
                    value={dp.value}
                    dataType={dp.dataType}
                    unit={dp.unit}
                    trend={dp.trend}
                    stale={isStale}
                    justUpdated={dp.justUpdated}
                  />
                </TableCell>
                <TableCell><QualityBadge quality={dp.quality} /></TableCell>
                <TableCell><DpStatusBadge status={dp.status} /></TableCell>
                <TableCell>
                  <span className="flex items-center gap-1 font-mono text-xs text-muted-foreground">
                    {dp.timestamp}
                    <StaleDataTag ageSeconds={dp.ageSeconds} />
                  </span>
                </TableCell>
                <TableCell className="text-xs text-muted-foreground">{dp.transportName}</TableCell>
                <TableCell className="font-mono text-xs text-muted-foreground">{dp.address}</TableCell>
                <TableCell>
                  <div className="flex items-center justify-end gap-0.5" onClick={(e) => e.stopPropagation()}>
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <Button variant="ghost" size="icon" className="size-7"
                          onClick={() => onTrend(dp)}>
                          <LineChart className="size-3.5" />
                        </Button>
                      </TooltipTrigger>
                      <TooltipContent>查看趋势</TooltipContent>
                    </Tooltip>

                    <ReadNowButton
                      datapointId={dp.id}
                      canControl={canControl}
                      wsDisconnected={wsDisconnected}
                      onResult={(r) => onReadNow(dp, r)}
                    />

                    <Tooltip>
                      <TooltipTrigger asChild>
                        <Button variant="ghost" size="icon" className="size-7"
                          onClick={() => onRowClick(dp)}>
                          <Info className="size-3.5" />
                        </Button>
                      </TooltipTrigger>
                      <TooltipContent>详情</TooltipContent>
                    </Tooltip>
                  </div>
                </TableCell>
              </TableRow>
            );
          })}
        </TableBody>
      </Table>
    </div>
  );
}
