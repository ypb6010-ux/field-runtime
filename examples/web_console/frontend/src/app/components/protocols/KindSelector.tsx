import { Loader2, FileJson } from "lucide-react";
import type { KindSchema } from "../../transports";
import { Label } from "../ui/label";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "../ui/select";

/** Kind 下拉选择，选项来自 GET /transports/kinds */
export function KindSelector({
  kinds,
  value,
  onChange,
  loading,
  disabled,
  lockedHint,
}: {
  kinds: KindSchema[];
  value: string;
  onChange: (kind: string) => void;
  loading?: boolean;
  disabled?: boolean;
  lockedHint?: string;
}) {
  return (
    <div className="space-y-1.5">
      <Label htmlFor="kind" className="flex items-center gap-1">
        协议类型 Kind <span className="text-status-error">*</span>
      </Label>
      <Select value={value} onValueChange={onChange} disabled={disabled || loading}>
        <SelectTrigger id="kind" className="w-full">
          {loading ? (
            <span className="flex items-center gap-2 text-muted-foreground">
              <Loader2 className="size-4 animate-spin" />
              加载协议类型…
            </span>
          ) : (
            <SelectValue placeholder="请选择协议类型" />
          )}
        </SelectTrigger>
        <SelectContent>
          {kinds.map((k) => (
            <SelectItem key={k.kind} value={k.kind}>
              {k.label} <span className="text-xs text-muted-foreground">v{k.version}</span>
            </SelectItem>
          ))}
        </SelectContent>
      </Select>
      {lockedHint ? (
        <p className="text-xs text-muted-foreground">{lockedHint}</p>
      ) : (
        <p className="flex items-center gap-1 text-xs text-muted-foreground">
          <FileJson className="size-3" />
          选项来自 /transports/kinds，新增类型无需改前端
        </p>
      )}
    </div>
  );
}
