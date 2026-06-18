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

  const logs = [
    { t: transport.lastConnectedAt ?? "—", tone: "text-status-success", msg: "连接握手成功" },
    { t: "2026-06-18 10:30:11", tone: "text-muted-foreground", msg: "轮询读取 128 个寄存器，耗时 22ms" },
    ...(transport.lastError
      ? [{ t: "2026-06-18 09:58:30", tone: "text-status-error", msg: transport.lastError }]
      : []),
  ];

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
                <dt className="text-muted-foreground">最近连接</dt>
                <dd className="col-span-2">{transport.lastConnectedAt ?? "—"}</dd>
                <dt className="text-muted-foreground">关联点位</dt>
                <dd className="col-span-2 tabular-nums">{transport.pointCount}</dd>
                <dt className="text-muted-foreground">更新人</dt>
                <dd className="col-span-2">{transport.updatedBy}</dd>
                <dt className="text-muted-foreground">标签</dt>
                <dd className="col-span-2 flex flex-wrap gap-1">
                  {transport.tags.length
                    ? transport.tags.map((t) => (
                        <Badge key={t} variant="secondary">
                          {t}
                        </Badge>
                      ))
                    : "—"}
                </dd>
              </dl>
            </Section>

            <Separator />

            <Section title="最近连接日志">
              <ul className="space-y-1.5">
                {logs.map((l, i) => (
                  <li key={i} className="flex gap-2 text-xs">
                    <span className="shrink-0 font-mono text-muted-foreground">{l.t}</span>
                    <span className={l.tone}>{l.msg}</span>
                  </li>
                ))}
              </ul>
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
