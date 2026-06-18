import { useState } from "react";
import { toast } from "sonner";
import { ExternalLink, RefreshCw, Copy, AlertTriangle } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { Badge } from "../components/ui/badge";

const DOCS_URL = "/api/docs";
const GROUPS = ["Auth", "System", "Transports", "Datapoints", "Polling", "Conversion", "Data", "Config", "Users & Roles"];

export function ApiDocs() {
  const [key, setKey] = useState(0);
  const [failed, setFailed] = useState(false);

  return (
    <>
      <PageHeader
        title="API 文档"
        en="API Docs"
        description="查看后端 REST API 与调试接口。"
        actions={
          <>
            <Button variant="outline" size="sm" className="gap-1.5" onClick={() => window.open(DOCS_URL, "_blank")}><ExternalLink className="size-3.5" />在新窗口打开</Button>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => { setFailed(false); setKey((k) => k + 1); }}><RefreshCw className="size-3.5" />刷新</Button>
            <Button variant="ghost" size="sm" className="gap-1.5" onClick={() => { navigator.clipboard?.writeText(window.location.origin + DOCS_URL); toast.success("已复制地址"); }}><Copy className="size-3.5" />复制地址</Button>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-sm text-muted-foreground">API 分组：</span>
          {GROUPS.map((g) => <Badge key={g} variant="outline" className="bg-card">{g}</Badge>)}
        </div>

        {failed ? (
          <div className="flex flex-col items-center justify-center gap-3 rounded-md border border-dashed border-border bg-card py-20 text-center">
            <AlertTriangle className="size-8 text-amber-500" />
            <div className="text-sm font-medium">API 文档加载失败</div>
            <div className="text-sm text-muted-foreground">无法加载 <code>{DOCS_URL}</code>，请检查后端服务或文档配置。</div>
            <div className="flex gap-2">
              <Button variant="outline" size="sm" onClick={() => { setFailed(false); setKey((k) => k + 1); }}>重试</Button>
              <Button size="sm" onClick={() => window.open(DOCS_URL, "_blank")}>在新窗口打开</Button>
            </div>
          </div>
        ) : (
          <div className="overflow-hidden rounded-md border border-border bg-card">
            <iframe
              key={key}
              title="Swagger UI"
              src={DOCS_URL}
              className="h-[calc(100vh-220px)] w-full"
              onError={() => setFailed(true)}
            />
          </div>
        )}
      </div>
    </>
  );
}
