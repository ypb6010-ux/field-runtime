import { useState, type ReactNode } from "react";
import { ChevronDown, Pencil, FileJson2, Radar } from "lucide-react";
import type { Transport } from "../../transports";
import { getKindSchema, kindLabel } from "../../transports";
import { TransportStatusBadge } from "./TransportStatusBadge";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription } from "../ui/sheet";
import { Button } from "../ui/button";
import { Badge } from "../ui/badge";
import { Separator } from "../ui/separator";
import { ScrollArea } from "../ui/scroll-area";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "../ui/collapsible";

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="space-y-2">
      <h3 className="text-sm text-muted-foreground">{title}</h3>
      {children}
    </section>
  );
}

export function TransportDetailDrawer({
  transport,
  open,
  onOpenChange,
  canWrite,
  onEdit,
}: {
  transport: Transport | null;
  open: boolean;
  onOpenChange: (o: boolean) => void;
  canWrite: boolean;
  onEdit: (t: Transport) => void;
}) {
  const [schemaOpen, setSchemaOpen] = useState(false);
  if (!transport) return null;
  const schema = getKindSchema(transport.kind);

  const numericUpdatedAt = Number(transport.updatedAt);
  const updatedAt = transport.updatedAt
    ? new Date(
        Number.isFinite(numericUpdatedAt)
          ? numericUpdatedAt < 10_000_000_000
            ? numericUpdatedAt * 1000
            : numericUpdatedAt
          : transport.updatedAt,
      ).toLocaleString()
    : "—";

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent side="right" className="flex flex-col gap-0 p-0 sm:max-w-none" style={{ width: 560, maxWidth: "94vw" }}>
        <SheetHeader className="border-b border-border px-6 py-4">
          <div className="flex items-center justify-between gap-2">
            <SheetTitle>{transport.name}</SheetTitle>
            <Button
              size="sm"
              variant="outline"
              className="gap-1.5"
              disabled={!canWrite}
              onClick={() => onEdit(transport)}
            >
              <Pencil className="size-3.5" />
              编辑
            </Button>
          </div>
          <SheetDescription className="font-mono text-xs">{transport.endpoint}</SheetDescription>
        </SheetHeader>

        <ScrollArea className="flex-1">
          <div className="space-y-5 px-6 py-5">
            <Section title="当前状态">
              <div className="flex flex-wrap items-center gap-2">
                <TransportStatusBadge status={transport.status} />
                <Badge variant="outline">{kindLabel(transport.kind)}</Badge>
                <Badge variant={transport.enabled ? "secondary" : "outline"}>
                  {transport.enabled ? "已启用" : "已停用"}
                </Badge>
              </div>
            </Section>

            <Separator />

            <Section title="基本信息">
              <dl className="grid grid-cols-3 gap-y-2 text-sm">
                <dt className="text-muted-foreground">配置更新时间</dt>
                <dd className="col-span-2">{updatedAt}</dd>
                <dt className="text-muted-foreground">关联点位</dt>
                <dd className="col-span-2 tabular-nums">{transport.pointCount}</dd>
              </dl>
            </Section>

            <Separator />

            <Section title="连接说明">
              <p className="text-xs leading-relaxed text-muted-foreground">
                当前状态来自运行时快照。历史连接事件请在“事件日志”中按协议 ID 检索。
              </p>
            </Section>

            <Separator />

            <Section title="关联点位">
              <div className="flex items-center justify-between rounded-md border border-border bg-muted/30 px-3 py-2.5 text-sm">
                <span className="flex items-center gap-2">
                  <Radar className="size-4 text-muted-foreground" />
                  共 {transport.pointCount} 个采集点
                </span>
                <Button variant="link" size="sm" className="h-auto p-0">
                  查看点位
                </Button>
              </div>
            </Section>

            <Separator />

            <Section title="原始 Schema 摘要">
              <div className="rounded-md border border-border">
                <div className="flex items-center justify-between px-3 py-2 text-xs">
                  <span className="flex items-center gap-1.5 text-muted-foreground">
                    <FileJson2 className="size-3.5" />
                    {schema?.label} v{schema?.version} · {schema?.fields.length} 字段
                  </span>
                </div>
                <Collapsible open={schemaOpen} onOpenChange={setSchemaOpen}>
                  <CollapsibleTrigger className="flex w-full items-center justify-between border-t border-border px-3 py-2 text-xs hover:bg-muted/40">
                    查看 Schema
                    <ChevronDown className={`size-3.5 transition-transform ${schemaOpen ? "rotate-180" : ""}`} />
                  </CollapsibleTrigger>
                  <CollapsibleContent>
                    <pre className="overflow-x-auto border-t border-border bg-muted/30 px-3 py-2 font-mono text-[11px] leading-relaxed text-muted-foreground">
{JSON.stringify(
  schema?.fields.map((f) => ({ name: f.name, type: f.type, required: !!f.required })),
  null,
  2,
)}
                    </pre>
                  </CollapsibleContent>
                </Collapsible>
              </div>
            </Section>
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  );
}
