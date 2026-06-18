import { Search, RotateCcw } from "lucide-react";
import type { DpStatus, DataType, Quality } from "../../live";
import { LIVE_TRANSPORTS } from "../../live";
import { Input } from "../ui/input";
import { Button } from "../ui/button";
import { Switch } from "../ui/switch";
import { Label } from "../ui/label";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../ui/select";

export interface LiveFilters {
  keyword: string;
  transportId: string;
  status: string;
  dataType: string;
  quality: string;
  onlyChanging: boolean;
}

export const EMPTY_LIVE_FILTERS: LiveFilters = {
  keyword: "", transportId: "all", status: "all", dataType: "all", quality: "all", onlyChanging: false,
};

const STATUSES: { value: DpStatus | "all"; label: string }[] = [
  { value: "all", label: "全部状态" },
  { value: "normal", label: "正常" },
  { value: "alarm", label: "告警" },
  { value: "error", label: "错误" },
  { value: "offline", label: "离线" },
  { value: "stale", label: "过期" },
];

const DATA_TYPES: { value: DataType | "all"; label: string }[] = [
  { value: "all", label: "全部类型" },
  { value: "number", label: "number" },
  { value: "boolean", label: "boolean" },
  { value: "string", label: "string" },
];

const QUALITIES: { value: Quality | "all"; label: string }[] = [
  { value: "all", label: "全部质量" },
  { value: "Good", label: "Good" },
  { value: "Uncertain", label: "Uncertain" },
  { value: "Bad", label: "Bad" },
];

export function LiveFilterBar({
  filters, onChange, onReset,
}: {
  filters: LiveFilters;
  onChange: (f: LiveFilters) => void;
  onReset: () => void;
}) {
  const set = (patch: Partial<LiveFilters>) => onChange({ ...filters, ...patch });

  return (
    <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-card p-3">
      {/* 关键字搜索 */}
      <div className="relative min-w-52 flex-1">
        <Search className="absolute left-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
        <Input
          value={filters.keyword}
          onChange={(e) => set({ keyword: e.target.value })}
          placeholder="搜索名称、地址、标签"
          className="pl-8"
        />
      </div>

      <Select value={filters.transportId} onValueChange={(v) => set({ transportId: v })}>
        <SelectTrigger className="w-44">
          <SelectValue placeholder="全部 Transport" />
        </SelectTrigger>
        <SelectContent>
          <SelectItem value="all">全部 Transport</SelectItem>
          {LIVE_TRANSPORTS.map((t) => (
            <SelectItem key={t.id} value={t.id}>{t.name}</SelectItem>
          ))}
        </SelectContent>
      </Select>

      <Select value={filters.status} onValueChange={(v) => set({ status: v })}>
        <SelectTrigger className="w-28">
          <SelectValue placeholder="状态" />
        </SelectTrigger>
        <SelectContent>
          {STATUSES.map((s) => <SelectItem key={s.value} value={s.value}>{s.label}</SelectItem>)}
        </SelectContent>
      </Select>

      <Select value={filters.dataType} onValueChange={(v) => set({ dataType: v })}>
        <SelectTrigger className="w-32">
          <SelectValue placeholder="类型" />
        </SelectTrigger>
        <SelectContent>
          {DATA_TYPES.map((d) => <SelectItem key={d.value} value={d.value}>{d.label}</SelectItem>)}
        </SelectContent>
      </Select>

      <Select value={filters.quality} onValueChange={(v) => set({ quality: v })}>
        <SelectTrigger className="w-32">
          <SelectValue placeholder="质量" />
        </SelectTrigger>
        <SelectContent>
          {QUALITIES.map((q) => <SelectItem key={q.value} value={q.value}>{q.label}</SelectItem>)}
        </SelectContent>
      </Select>

      <div className="flex items-center gap-2 rounded-md border border-border px-2.5 py-2">
        <Switch
          id="only-changing"
          checked={filters.onlyChanging}
          onCheckedChange={(c) => set({ onlyChanging: c })}
          className="scale-75"
        />
        <Label htmlFor="only-changing" className="cursor-pointer text-xs">只看变化中</Label>
      </div>

      <Button variant="ghost" size="sm" className="gap-1.5" onClick={onReset}>
        <RotateCcw className="size-3.5" />
        重置
      </Button>
    </div>
  );
}
