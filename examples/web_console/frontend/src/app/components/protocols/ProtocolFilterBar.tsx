import { Search, RotateCcw } from "lucide-react";
import type { KindSchema, TransportStatus } from "../../transports";
import { STATUS_META } from "../../transports";
import { Input } from "../ui/input";
import { Button } from "../ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "../ui/select";

export interface Filters {
  keyword: string;
  kind: string; // "all" | kind
  status: string; // "all" | TransportStatus
}

const STATUSES = Object.keys(STATUS_META) as TransportStatus[];

export function ProtocolFilterBar({
  filters,
  kinds,
  onChange,
  onReset,
}: {
  filters: Filters;
  kinds: KindSchema[];
  onChange: (next: Filters) => void;
  onReset: () => void;
}) {
  const set = (patch: Partial<Filters>) => onChange({ ...filters, ...patch });

  return (
    <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
      <div className="relative min-w-56 flex-1">
        <Search className="absolute left-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
        <Input
          value={filters.keyword}
          onChange={(e) => set({ keyword: e.target.value })}
          placeholder="搜索名称、ID 或 Endpoint"
          className="pl-8"
        />
      </div>

      <Select value={filters.kind} onValueChange={(v) => set({ kind: v })}>
        <SelectTrigger className="w-40">
          <SelectValue placeholder="协议类型" />
        </SelectTrigger>
        <SelectContent>
          <SelectItem value="all">全部类型</SelectItem>
          {kinds.map((k) => (
            <SelectItem key={k.kind} value={k.kind}>
              {k.label}
            </SelectItem>
          ))}
        </SelectContent>
      </Select>

      <Select value={filters.status} onValueChange={(v) => set({ status: v })}>
        <SelectTrigger className="w-32">
          <SelectValue placeholder="状态" />
        </SelectTrigger>
        <SelectContent>
          <SelectItem value="all">全部状态</SelectItem>
          {STATUSES.map((s) => (
            <SelectItem key={s} value={s}>
              {STATUS_META[s].label}
            </SelectItem>
          ))}
        </SelectContent>
      </Select>

      <Button variant="ghost" className="gap-1.5" onClick={onReset}>
        <RotateCcw className="size-3.5" />
        重置
      </Button>
    </div>
  );
}
