import { useEffect, useState } from "react";
import { Loader2, PlugZap, Lock, AlertTriangle } from "lucide-react";
import type { KindSchema, Transport, TransportPayload } from "../../transports";
import { getKindSchema, testConnection } from "../../transports";
import type { FieldValue } from "./schema-fields";
import { KindSelector } from "./KindSelector";
import { SchemaFormRenderer } from "./SchemaFormRenderer";
import { SchemaLoadErrorState } from "./SchemaLoadErrorState";
import { TestConnectionPanel, type TestState } from "./TestConnectionPanel";
import { DangerousConfirmModal } from "./DangerousConfirmModal";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription } from "../ui/sheet";
import { Input } from "../ui/input";
import { Label } from "../ui/label";
import { Switch } from "../ui/switch";
import { Button } from "../ui/button";
import { Separator } from "../ui/separator";
import { ScrollArea } from "../ui/scroll-area";
import { Alert, AlertDescription, AlertTitle } from "../ui/alert";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";
import { isValidResourceId } from "../../api";

interface TransportDrawerProps {
  open: boolean;
  onOpenChange: (o: boolean) => void;
  mode: "create" | "edit";
  initial?: Transport | null;
  kinds: KindSchema[];
  kindsLoading: boolean;
  kindsError: string | null;
  onRetryKinds: () => void;
  onSave: (mode: "create" | "edit", payload: TransportPayload) => Promise<void>;
}

function buildDefaults(schema?: KindSchema): Record<string, FieldValue> {
  const out: Record<string, FieldValue> = {};
  schema?.fields.forEach((f) => {
    if (f.default !== undefined) out[f.name] = f.default;
  });
  return out;
}

function isFilled(type: string, v: FieldValue, savedSecret: boolean) {
  if (type === "secret" && savedSecret) return true;
  if (v === undefined || v === null) return false;
  if (typeof v === "string") return v.trim() !== "";
  return true;
}

const ENDPOINT_FIELDS = ["host", "broker_url", "endpoint_url", "base_url", "serial_port"];
function primaryEndpointOf(values: Record<string, FieldValue>): string {
  const f = ENDPOINT_FIELDS.find((n) => values[n]);
  return f ? String(values[f]) : "—";
}

export function TransportDrawer({
  open,
  onOpenChange,
  mode,
  initial,
  kinds,
  kindsLoading,
  kindsError,
  onRetryKinds,
  onSave,
}: TransportDrawerProps) {
  const [id, setId] = useState("");
  const [name, setName] = useState("");
  const [enabled, setEnabled] = useState(true);

  const [kind, setKind] = useState("");
  const [values, setValues] = useState<Record<string, FieldValue>>({});
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [savedSecrets, setSavedSecrets] = useState<string[]>([]);
  const [kindLocked, setKindLocked] = useState(false);
  const [unlockAsk, setUnlockAsk] = useState(false);

  const [testState, setTestState] = useState<TestState>({ status: "idle" });
  const [saving, setSaving] = useState(false);

  const schema = getKindSchema(kind);

  // 打开时初始化
  useEffect(() => {
    if (!open) return;
    setErrors({});
    setTestState({ status: "idle" });
    if (mode === "edit" && initial) {
      const sc = getKindSchema(initial.kind);
      setId(initial.id);
      setName(initial.name);
      setEnabled(initial.enabled);
      setKind(initial.kind);
      setValues({ ...initial.config });
      // 编辑态：secret 字段脱敏，不回显
      setSavedSecrets(sc?.fields.filter((f) => f.type === "secret").map((f) => f.name) ?? []);
      setKindLocked(true);
      setTestState({ status: "idle" });
    } else {
      const first = kinds[0]?.kind ?? "";
      setId("");
      setName("");
      setEnabled(true);
      setKind(first);
      setValues(buildDefaults(getKindSchema(first)));
      setSavedSecrets([]);
      setKindLocked(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, mode, initial?.id]);

  function handleKindChange(next: string) {
    setKind(next);
    setValues(buildDefaults(getKindSchema(next)));
    setErrors({});
    setTestState({ status: "idle" });
    setSavedSecrets([]);
  }

  function setFieldValue(fieldName: string, v: FieldValue) {
    setValues((prev) => ({ ...prev, [fieldName]: v }));
    setErrors((prev) => {
      if (!prev[fieldName]) return prev;
      const next = { ...prev };
      delete next[fieldName];
      return next;
    });
    // secret 改动后不再视为已保存脱敏
    if (savedSecrets.includes(fieldName)) {
      setSavedSecrets((s) => s.filter((n) => n !== fieldName));
    }
  }

  function validate(): boolean {
    const e: Record<string, string> = {};
    if (!isValidResourceId(id)) {
      e.__id = "ID 须为 1–128 字符，且不可含空白或 /\\?#%";
    }
    if (name.trim() === "") e.__name = "请输入协议名称";
    schema?.fields.forEach((f) => {
      if (f.required && !isFilled(f.type, values[f.name], savedSecrets.includes(f.name))) {
        e[f.name] = `${f.title} 为必填项`;
      }
      if (f.type === "number" && typeof values[f.name] === "number") {
        const value = values[f.name] as number;
        if (!Number.isFinite(value)) e[f.name] = `${f.title} 必须是有效数字`;
        else if (f.minimum !== undefined && value < f.minimum) e[f.name] = `${f.title} 不能小于 ${f.minimum}`;
        else if (f.maximum !== undefined && value > f.maximum) e[f.name] = `${f.title} 不能大于 ${f.maximum}`;
      }
    });
    setErrors(e);
    return Object.keys(e).length === 0;
  }

  function missingRequiredTitles(): string[] {
    if (!schema) return [];
    return schema.fields
      .filter((f) => f.required && !isFilled(f.type, values[f.name], savedSecrets.includes(f.name)))
      .map((f) => f.title);
  }

  async function handleTest() {
    const missing = missingRequiredTitles();
    if (missing.length > 0) {
      setTestState({ status: "missing", missing });
      return;
    }
    setTestState({ status: "loading", endpoint: primaryEndpointOf(values) });
    try {
      const result = await testConnection(kind, values);
      setTestState({ status: result.ok ? "success" : "error", result });
    } catch (error) {
      setTestState({
        status: "error",
        result: {
          ok: false,
          endpoint: primaryEndpointOf(values),
          at: new Date().toLocaleTimeString("zh-CN", { hour12: false }),
          message: error instanceof Error ? error.message : "连接测试失败",
        },
      });
    }
  }

  async function handleSave() {
    if (!validate()) return;
    setSaving(true);
    try {
      const params: Record<string, string | number | boolean> = {};
      schema?.fields.forEach((field) => {
        const value = values[field.name];
        if (
          typeof value === "string"
          || typeof value === "number"
          || typeof value === "boolean"
        ) {
          params[field.name] = value;
        }
      });
      await onSave(mode, {
        id: id.trim(),
        name: name.trim(),
        kind,
        enabled,
        params_json: params,
        scheduler_json: mode === "edit" ? initial?.scheduler ?? {} : {},
      });
      onOpenChange(false);
    } catch (error) {
      setErrors((current) => ({
        ...current,
        __save: error instanceof Error ? error.message : "保存失败",
      }));
    } finally {
      setSaving(false);
    }
  }

  const canSave = !kindsError && !kindsLoading && !!schema;

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent
        side="right"
        className="flex flex-col gap-0 p-0 sm:max-w-none"
        style={{ width: 720, maxWidth: "94vw" }}
      >
        <SheetHeader className="border-b border-border px-6 py-4">
          <SheetTitle>{mode === "edit" ? "编辑协议连接" : "新增协议连接"}</SheetTitle>
          <SheetDescription>
            {mode === "edit"
              ? "修改后保存为草稿，需要发布后才会影响运行配置。"
              : "填写基本信息并选择协议类型，配置表单由后端 Schema 动态生成。"}
          </SheetDescription>
        </SheetHeader>

        <ScrollArea className="min-h-0 flex-1">
          <div className="space-y-6 px-6 py-5">
            {/* 编辑态：草稿生效提示 */}
            {mode === "edit" && (
              <Alert className="border-status-warning-border bg-status-warning-bg text-status-warning">
                <AlertTriangle className="size-4" />
                <AlertTitle>修改后不会立即生效</AlertTitle>
                <AlertDescription className="text-status-warning/80">
                  修改协议连接后不会立即生效，保存后会产生 draft config diff，需要到 Config & Apply 发布后才会影响运行配置。
                </AlertDescription>
              </Alert>
            )}

            {/* 编辑态：最近连接错误（已移至 D 区域展示，此处不重复） */}

            {/* A. 基本信息 */}
            <section className="space-y-4">
              <h3 className="text-sm text-muted-foreground">A · 基本信息</h3>
              <div className="space-y-1.5">
                <Label htmlFor="t-id" className="flex items-center gap-1">
                  连接 ID <span className="text-status-error">*</span>
                </Label>
                <Input
                  id="t-id"
                  value={id}
                  disabled={mode === "edit"}
                  onChange={(event) => setId(event.target.value)}
                  placeholder="如 workshop-a-plc-01"
                  aria-invalid={!!errors.__id}
                />
                {errors.__id && <p className="text-xs text-status-error">{errors.__id}</p>}
              </div>
              <div className="space-y-1.5">
                <Label htmlFor="t-name" className="flex items-center gap-1">
                  协议名称 <span className="text-status-error">*</span>
                </Label>
                <Input
                  id="t-name"
                  value={name}
                  onChange={(e) => {
                    setName(e.target.value);
                    if (errors.__name) setErrors((p) => ({ ...p, __name: "" }));
                  }}
                  placeholder="如：车间 A · PLC-01"
                  aria-invalid={!!errors.__name}
                />
                {errors.__name && <p className="text-xs text-status-error">{errors.__name}</p>}
              </div>
              <div className="flex items-center justify-between rounded-md border border-border px-3 py-2.5">
                <Label htmlFor="t-enabled">启用连接</Label>
                <Switch id="t-enabled" checked={enabled} onCheckedChange={setEnabled} />
              </div>
            </section>

            <Separator />

            {/* B. 协议类型 */}
            <section className="space-y-4">
              <h3 className="text-sm text-muted-foreground">B · 协议类型 Kind</h3>
              {kindsError ? (
                <SchemaLoadErrorState message={kindsError} onRetry={onRetryKinds} retrying={kindsLoading} />
              ) : mode === "edit" && kindLocked ? (
                <div className="space-y-2">
                  <KindSelector
                    kinds={kinds}
                    value={kind}
                    onChange={() => {}}
                    disabled
                    lockedHint="如需修改协议类型，请新建连接或进行二次确认。"
                  />
                  <Button variant="link" size="sm" className="h-auto gap-1 p-0" onClick={() => setUnlockAsk(true)}>
                    <Lock className="size-3.5" />
                    更换类型
                  </Button>
                </div>
              ) : (
                <KindSelector kinds={kinds} value={kind} onChange={handleKindChange} loading={kindsLoading} />
              )}
            </section>

            {/* C. 动态配置 Schema Form */}
            {!kindsError && (
              <section className="space-y-4">
                <h3 className="text-sm text-muted-foreground">C · 协议配置（动态）</h3>
                {kindsLoading ? (
                  <div className="flex items-center gap-2 rounded-md border border-border bg-muted/30 px-4 py-6 text-sm text-muted-foreground">
                    <Loader2 className="size-4 animate-spin text-primary" />
                    正在加载协议 Schema…
                  </div>
                ) : schema ? (
                  <SchemaFormRenderer
                    schema={schema}
                    values={values}
                    errors={errors}
                    savedSecrets={savedSecrets}
                    onChange={setFieldValue}
                  />
                ) : null}
              </section>
            )}

            <Separator />

            {/* D. 测试连接结果 */}
            <section className="space-y-3">
              <h3 className="text-sm text-muted-foreground">D · 测试连接</h3>

              {/* 编辑态：最近状态摘要 */}
              {mode === "edit" && initial && (
                <div className="rounded-md border border-border bg-muted/30 px-4 py-3">
                  <p className="mb-2 text-xs text-muted-foreground">最近状态（运行时记录）</p>
                  <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1.5 text-xs">
                    <dt className="text-muted-foreground">运行状态</dt>
                    <dd className="tabular-nums">{initial.status}</dd>
                  </dl>
                </div>
              )}

              <TestConnectionPanel state={testState} />
            </section>
          </div>
        </ScrollArea>

        {/* 底部操作（固定，右对齐） */}
        <div className="flex items-center justify-end gap-2 border-t border-border px-6 py-3">
          {errors.__save && (
            <span className="mr-auto text-xs text-status-error">{errors.__save}</span>
          )}
          {/* 测试连接：kindsError 时禁用并提示 */}
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="inline-flex">
                <Button
                  variant="outline"
                  className="gap-1.5"
                  disabled={!canSave || testState.status === "loading"}
                  onClick={handleTest}
                >
                  {testState.status === "loading" ? (
                    <Loader2 className="size-4 animate-spin" />
                  ) : (
                    <PlugZap className="size-4" />
                  )}
                  测试连接
                </Button>
              </span>
            </TooltipTrigger>
            {(!canSave && !kindsLoading) && (
              <TooltipContent>请先加载协议类型 Schema</TooltipContent>
            )}
          </Tooltip>

          <Button variant="ghost" onClick={() => onOpenChange(false)}>
            取消
          </Button>

          {/* 保存草稿：kindsError 时禁用并提示 */}
          <Tooltip>
            <TooltipTrigger asChild>
              <span className="inline-flex">
                <Button disabled={!canSave || saving} onClick={handleSave}>
                  {saving && <Loader2 className="size-4 animate-spin" />}
                  保存草稿
                </Button>
              </span>
            </TooltipTrigger>
            {(!canSave && !kindsLoading) && (
              <TooltipContent>请先加载协议类型 Schema</TooltipContent>
            )}
          </Tooltip>
        </div>
      </SheetContent>

      {/* 编辑态更换类型确认 */}
      <DangerousConfirmModal
        open={unlockAsk}
        onOpenChange={setUnlockAsk}
        title="确认更换协议类型？"
        description="更换 Kind 会清空当前协议配置并按新类型重新填写，已关联的点位映射可能失效。"
        confirmText="确认更换"
        onConfirm={() => {
          setKindLocked(false);
          setUnlockAsk(false);
        }}
      />
    </Sheet>
  );
}
