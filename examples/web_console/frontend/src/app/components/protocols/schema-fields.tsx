import { useState, type ReactNode } from "react";
import { Eye, EyeOff, Info, KeyRound, AlertTriangle } from "lucide-react";
import type { SchemaFieldDef } from "../../transports";
import { Input } from "../ui/input";
import { Textarea } from "../ui/textarea";
import { Switch } from "../ui/switch";
import { Label } from "../ui/label";
import { Button } from "../ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "../ui/select";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";

export type FieldValue = string | number | boolean | undefined;

/** 字段外壳：标签（必填星号 + 说明 tooltip）+ 控件 + helper/错误 */
function FieldShell({
  field,
  htmlFor,
  control,
  error,
}: {
  field: SchemaFieldDef;
  htmlFor: string;
  control: ReactNode;
  error?: string;
}) {
  return (
    <div className="space-y-1.5">
      <div className="flex items-center gap-1.5">
        <Label htmlFor={htmlFor} className="flex items-center gap-1">
          {field.title}
          {field.required && <span className="text-status-error">*</span>}
        </Label>
        {field.description && (
          <Tooltip>
            <TooltipTrigger asChild>
              <Info className="size-3.5 cursor-help text-muted-foreground" />
            </TooltipTrigger>
            <TooltipContent>{field.description}</TooltipContent>
          </Tooltip>
        )}
        {field.unit && <span className="text-xs text-muted-foreground">（{field.unit}）</span>}
      </div>
      {control}
      {error ? (
        <p className="text-xs text-status-error">{error}</p>
      ) : (
        field.default !== undefined && (
          <p className="text-xs text-muted-foreground">默认值：{String(field.default)}</p>
        )
      )}
    </div>
  );
}

/** 脱敏密码字段：新增时可用眼睛查看；编辑时显示“已保存，点击重新设置” */
export function SecretField({
  field,
  value,
  onChange,
  savedMasked,
  error,
}: {
  field: SchemaFieldDef;
  value: FieldValue;
  onChange: (v: string) => void;
  savedMasked?: boolean;
  error?: string;
}) {
  const id = `f-${field.name}`;
  const [reveal, setReveal] = useState(false);
  const [editing, setEditing] = useState(!savedMasked);

  const control =
    savedMasked && !editing ? (
      <div className="flex items-center justify-between rounded-md border border-border bg-muted/40 px-3 py-2">
        <span className="flex items-center gap-2 text-sm text-muted-foreground">
          <KeyRound className="size-3.5" />
          已保存，出于安全不回显
        </span>
        <Button
          type="button"
          variant="link"
          size="sm"
          className="h-auto p-0"
          onClick={() => {
            setEditing(true);
            onChange("");
          }}
        >
          重新设置
        </Button>
      </div>
    ) : (
      <div className="relative">
        <Input
          id={id}
          type={reveal ? "text" : "password"}
          value={(value as string) ?? ""}
          placeholder={field.placeholder ?? "••••••••"}
          onChange={(e) => onChange(e.target.value)}
          className="pr-9"
          aria-invalid={!!error}
        />
        <button
          type="button"
          onClick={() => setReveal((r) => !r)}
          className="absolute right-2 top-1/2 -translate-y-1/2 text-muted-foreground hover:text-foreground"
          aria-label={reveal ? "隐藏" : "显示"}
        >
          {reveal ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
        </button>
      </div>
    );

  return <FieldShell field={field} htmlFor={id} control={control} error={error} />;
}

/** 不支持的字段类型 fallback */
export function UnsupportedSchemaField({ field }: { field: SchemaFieldDef }) {
  return (
    <div className="space-y-1.5">
      <Label className="flex items-center gap-1 text-muted-foreground">
        {field.title}
        <span className="text-xs">（{field.rawType ?? "unknown"}）</span>
      </Label>
      <div className="flex items-start gap-2 rounded-md border border-dashed border-status-warning-border bg-status-warning-bg px-3 py-2.5 text-xs text-status-warning">
        <AlertTriangle className="mt-0.5 size-3.5 shrink-0" />
        <span>
          Unsupported field type：前端暂不支持渲染该字段（{field.rawType ?? "unknown"}），
          请在后端确认或升级前端 Schema Renderer。该字段将以原始值提交。
        </span>
      </div>
    </div>
  );
}

/** 单个 Schema 字段渲染器，按 type 分发 */
export function SchemaField({
  field,
  value,
  onChange,
  savedMasked,
  error,
}: {
  field: SchemaFieldDef;
  value: FieldValue;
  onChange: (v: FieldValue) => void;
  savedMasked?: boolean;
  error?: string;
}) {
  const id = `f-${field.name}`;

  switch (field.type) {
    case "string":
      return (
        <FieldShell
          field={field}
          htmlFor={id}
          error={error}
          control={
            <Input
              id={id}
              value={(value as string) ?? ""}
              placeholder={field.placeholder}
              onChange={(e) => onChange(e.target.value)}
              aria-invalid={!!error}
            />
          }
        />
      );

    case "number":
      return (
        <FieldShell
          field={field}
          htmlFor={id}
          error={error}
          control={
            <Input
              id={id}
              type="number"
              min={field.minimum}
              max={field.maximum}
              value={value === undefined ? "" : (value as number)}
              placeholder={field.default !== undefined ? String(field.default) : field.placeholder}
              onChange={(e) => onChange(e.target.value === "" ? undefined : Number(e.target.value))}
              aria-invalid={!!error}
            />
          }
        />
      );

    case "boolean":
      return (
        <div className="flex items-center justify-between rounded-md border border-border px-3 py-2.5">
          <div className="flex items-center gap-1.5">
            <Label htmlFor={id}>{field.title}</Label>
            {field.description && (
              <Tooltip>
                <TooltipTrigger asChild>
                  <Info className="size-3.5 cursor-help text-muted-foreground" />
                </TooltipTrigger>
                <TooltipContent>{field.description}</TooltipContent>
              </Tooltip>
            )}
          </div>
          <Switch id={id} checked={!!value} onCheckedChange={(c) => onChange(c)} />
        </div>
      );

    case "enum":
      return (
        <FieldShell
          field={field}
          htmlFor={id}
          error={error}
          control={
            <Select value={(value as string) ?? ""} onValueChange={(v) => onChange(v)}>
              <SelectTrigger id={id} className="w-full" aria-invalid={!!error}>
                <SelectValue placeholder="请选择" />
              </SelectTrigger>
              <SelectContent>
                {field.options?.map((o) => (
                  <SelectItem key={o.value} value={o.value}>
                    {o.label}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          }
        />
      );

    case "secret":
      return (
        <SecretField field={field} value={value} onChange={onChange} savedMasked={savedMasked} error={error} />
      );

    case "text":
      return (
        <FieldShell
          field={field}
          htmlFor={id}
          error={error}
          control={
            <Textarea
              id={id}
              rows={3}
              value={(value as string) ?? ""}
              placeholder={field.placeholder}
              onChange={(e) => onChange(e.target.value)}
              className="font-mono text-xs"
            />
          }
        />
      );

    case "object":
    case "array":
      return (
        <FieldShell
          field={field}
          htmlFor={id}
          error={error}
          control={
            <Textarea
              id={id}
              rows={3}
              value={(value as string) ?? ""}
              placeholder={field.type === "array" ? "[]" : "{}"}
              onChange={(e) => onChange(e.target.value)}
              className="font-mono text-xs"
            />
          }
        />
      );

    case "unsupported":
    default:
      return <UnsupportedSchemaField field={field} />;
  }
}
