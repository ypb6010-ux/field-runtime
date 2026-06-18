import { useState } from "react";
import { ChevronDown, FileJson2, AlertTriangle, Settings2 } from "lucide-react";
import type { KindSchema, SchemaFieldDef } from "../../transports";
import { SchemaField, type FieldValue } from "./schema-fields";
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "../ui/collapsible";

function fullWidth(f: SchemaFieldDef) {
  return ["boolean", "text", "object", "array", "secret", "unsupported"].includes(f.type);
}

/**
 * 根据后端 JSON Schema 动态渲染协议配置表单。
 * 普通字段进网格，advanced 字段进“高级配置”折叠区，不支持的类型走 fallback。
 */
export function SchemaFormRenderer({
  schema,
  values,
  errors,
  savedSecrets = [],
  onChange,
}: {
  schema: KindSchema;
  values: Record<string, FieldValue>;
  errors: Record<string, string>;
  savedSecrets?: string[];
  onChange: (name: string, v: FieldValue) => void;
}) {
  const [advancedOpen, setAdvancedOpen] = useState(false);

  const basic = schema.fields.filter((f) => !f.advanced);
  const advanced = schema.fields.filter((f) => f.advanced);
  const hasUnsupported = schema.fields.some((f) => f.type === "unsupported");

  const renderField = (f: SchemaFieldDef) => (
    <div key={f.name} className={fullWidth(f) ? "sm:col-span-2" : ""}>
      <SchemaField
        field={f}
        value={values[f.name]}
        error={errors[f.name]}
        savedMasked={savedSecrets.includes(f.name)}
        onChange={(v) => onChange(f.name, v)}
      />
    </div>
  );

  return (
    <div className="space-y-4">
      {/* 轻量说明：当前表单由后端 Schema 动态渲染 */}
      <div className="flex items-center gap-2 rounded-md bg-secondary px-3 py-2 text-xs text-primary">
        <FileJson2 className="size-3.5" />
        Schema Loaded from <span className="font-mono">/transports/kinds</span> · {schema.label} v
        {schema.version} · 当前表单由后端 Schema 动态渲染
      </div>

      {/* Schema 不兼容警告 */}
      {hasUnsupported && (
        <div className="flex items-start gap-2 rounded-md border border-status-warning-border bg-status-warning-bg px-3 py-2.5 text-xs text-status-warning">
          <AlertTriangle className="mt-0.5 size-3.5 shrink-0" />
          该协议 Schema 含前端暂不支持的字段，支持的字段正常显示，不支持的字段需手动确认。
        </div>
      )}

      {/* 基础字段网格 */}
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">{basic.map(renderField)}</div>

      {/* 高级配置折叠区（array/object 等） */}
      {advanced.length > 0 && (
        <Collapsible open={advancedOpen} onOpenChange={setAdvancedOpen}>
          <CollapsibleTrigger className="flex w-full items-center justify-between rounded-md border border-border bg-muted/30 px-3 py-2 text-sm hover:bg-muted/50">
            <span className="flex items-center gap-2">
              <Settings2 className="size-4 text-muted-foreground" />
              高级配置（{advanced.length} 项）
            </span>
            <ChevronDown
              className={`size-4 text-muted-foreground transition-transform ${advancedOpen ? "rotate-180" : ""}`}
            />
          </CollapsibleTrigger>
          <CollapsibleContent className="pt-4">
            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">{advanced.map(renderField)}</div>
          </CollapsibleContent>
        </Collapsible>
      )}
    </div>
  );
}
