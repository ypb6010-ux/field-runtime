import { useEffect, useState } from "react";
import { Loader2, PlugZap, Lock, AlertTriangle } from "lucide-react";
import type { KindSchema, Transport } from "../../transports";
import { getKindSchema, testConnection } from "../../transports";
import type { FieldValue } from "./schema-fields";
import { KindSelector } from "./KindSelector";
import { SchemaFormRenderer } from "./SchemaFormRenderer";
import { SchemaLoadErrorState } from "./SchemaLoadErrorState";
import { TestConnectionPanel, type TestState } from "./TestConnectionPanel";
import { DangerousConfirmModal } from "./DangerousConfirmModal";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetDescription } from "../ui/sheet";
import { Input } from "../ui/input";
import { Textarea } from "../ui/textarea";
import { Label } from "../ui/label";
import { Switch } from "../ui/switch";
import { Button } from "../ui/button";
import { Badge } from "../ui/badge";
import { Separator } from "../ui/separator";
import { ScrollArea } from "../ui/scroll-area";
import { Alert, AlertDescription, AlertTitle } from "../ui/alert";
import { Tooltip, TooltipContent, TooltipTrigger } from "../ui/tooltip";

interface TransportDrawerProps {
  open: boolean;
  onOpenChange: (o: boolean) => void;
  mode: "create" | "edit";
  initial?: Transport | null;
  kinds: KindSchema[];
  kindsLoading: boolean;
  kindsError: string | null;
  onRetryKinds: () => void;
  onSaved: (mode: "create" | "edit") => void;
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

/** 编辑态：根据连接当前状态构造最近一次测试结果 */
function seedTestResult(t: Transport): TestState {
  if (t.status === "error" || t.lastError) {
    return {
      status: "error",
      result: {
        ok: false,
        endpoint: t.endpoint,
        at: t.lastConnectedAt ?? "—",
        message: t.lastError ?? "上次测试失败",
        errorType: "Timeout",
        suggestions: [
          "Host / Broker URL 是否正确",
          "Port 是否开放",
          "认证信息是否正确",
          "网络是否可达",
          "防火墙是否放行",
        ],
      },
    };
  }
  return {
    status: "success",
    result: { ok: true, latencyMs: 24, endpoint: t.endpoint, at: t.lastConnectedAt ?? "—", message: "connection accepted" },
  };
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
  onSaved,
}: TransportDrawerProps) {
  const [name, setName] = useState("");
  const [note, setNote] = useState("");
  const [tags, setTags] = useState<string[]>([]);
  const [tagInput, setTagInput] = useState("");
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
      setName(initial.name);
      setNote("");
      setTags(initial.tags);
      setEnabled(initial.enabled);
      setKind(initial.kind);
      setValues({ ...initial.config });
      // 编辑态：secret 字段脱敏，不回显
      setSavedSecrets(sc?.fields.filter((f) => f.type === "secret").map((f) => f.name) ?? []);
      setKindLocked(true);
      setTestState(seedTestResult(initial)); // 显示最近一次测试结果
    } else {
      const first = kinds[0]?.kind ?? "";
      setName("");
      setNote("");
      setTags([]);
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
    if (name.trim() === "") e.__name = "请输入协议名称";
    schema?.fields.forEach((f) => {
      if (f.required && !isFilled(f.type, values[f.name], savedSecrets.includes(f.name))) {
        e[f.name] = `${f.title} 为必填项`;
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
    const result = await testConnection(kind, values);
    setTestState({ status: result.ok ? "success" : "error", result });
  }

  async function handleSave() {
    if (!validate()) return;
    setSaving(true);
    await new Promise((r) => setTimeout(r, 600));
    setSaving(false);
    onSaved(mode);
    onOpenChange(false);
  }

  function addTag() {
    const t = tagInput.trim();
    if (t && !tags.includes(t)) setTags((p) => [...p, t]);
    setTagInput("");
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
              <div className="space-y-1.5">
                <Label htmlFor="t-note">备注</Label>
                <Textarea id="t-note" rows={2} value={note} onChange={(e) => setNote(e.target.value)} placeholder="可选" />
              </div>
              <div className="space-y-1.5">
                <Label htmlFor="t-tags">标签</Label>
                <div className="flex gap-2">
                  <Input
                    id="t-tags"
                    value={tagInput}
                    onChange={(e) => setTagInput(e.target.value)}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") {
                        e.preventDefault();
                        addTag();
                      }
                    }}
                    placeholder="输入后回车添加"
                  />
                  <Button type="button" variant="outline" onClick={addTag}>
                    添加
                  </Button>
                </div>
                {tags.length > 0 && (
                  <div className="flex flex-wrap gap-1.5 pt-1">
                    {tags.map((t) => (
                      <Badge
                        key={t}
                        variant="secondary"
                        className="cursor-pointer"
                        onClick={() => setTags((p) => p.filter((x) => x !== t))}
                      >
                        {t} ✕
                      </Badge>
                    ))}
                  </div>
                )}
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
                    <dt className="text-muted-foreground">最近连接时间</dt>
                    <dd className="tabular-nums">{initial.lastConnectedAt ?? "—"}</dd>
                    <dt className="text-muted-foreground">最近测试时间</dt>
                    <dd className="tabular-nums">{initial.lastConnectedAt ?? "—"}</dd>
                    <dt className="text-muted-foreground">最近测试结果</dt>
                    <dd>
                      {initial.status === "error" || initial.lastError ? (
                        <span className="text-status-error">失败</span>
                      ) : (
                        <span className="text-status-success">成功</span>
                      )}
                    </dd>
                    {initial.lastError && (
                      <>
                        <dt className="text-muted-foreground">最近连接错误</dt>
                        <dd className="break-all font-mono text-status-error">{initial.lastError}</dd>
                      </>
                    )}
                  </dl>
                </div>
              )}

              <TestConnectionPanel state={testState} />
            </section>
          </div>
        </ScrollArea>

        {/* 底部操作（固定，右对齐） */}
        <div className="flex items-center justify-end gap-2 border-t border-border px-6 py-3">
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
