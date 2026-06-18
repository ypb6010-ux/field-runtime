/**
 * StatePlayground — 状态演示页（设计说明，非生产业务页面）
 * 用于在当前骨架阶段快速切换并预览所有 Live 页面状态，
 * 包含 WS 状态切换、数据错误模拟、订阅失败模拟等。
 * 在真实产品中应移除或限定为开发模式访问。
 */
import { useState, type ReactNode } from "react";
import { Play, Pause, Wifi, WifiOff, RefreshCw, ServerCrash, AlertTriangle, CheckCircle2 } from "lucide-react";
import { toast } from "sonner";
import { PageHeader } from "../components/PageHeader";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import { Button } from "../components/ui/button";
import { Badge } from "../components/ui/badge";
import { Separator } from "../components/ui/separator";
import { WsStatusBanner, SubscriptionErrorBanner, LiveStatusBar } from "../components/live/LiveStatusBar";
import { LatestDataErrorState, LiveEmptyState } from "../components/live/LiveEmptyError";
import { ReadNowResultPanel } from "../components/live/ReadNowButton";
import { QualityBadge, DpStatusBadge, StaleDataTag } from "../components/live/atoms";
import type { WsState } from "../live";

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="space-y-3">
      <h3 className="text-xs uppercase tracking-wide text-muted-foreground">{title}</h3>
      {children}
    </section>
  );
}

export function StatePlayground() {
  const [wsState, setWsState] = useState<WsState>("connected");
  const [showSubErr, setShowSubErr] = useState(false);
  const [showLatestErr, setShowLatestErr] = useState(false);

  return (
    <>
      <PageHeader
        title="状态演示"
        en="State Playground"
        description="Live 页面所有状态的交互预览（设计说明页，不出现在生产导航中）。"
      />
      <div className="space-y-6 p-6">
        {/* WS 状态切换 */}
        <Section title="1 · WebSocket 状态切换">
          <Card className="gap-0">
            <div className="flex flex-wrap items-center gap-2 px-6 pt-6">
              <CardTitle className="text-sm">切换 WS 状态</CardTitle>
              <Badge variant={wsState === "connected" ? "default" : "outline"}
                className={wsState === "connected" ? "bg-status-success text-white" : wsState === "reconnecting" ? "border-status-warning-border bg-status-warning-bg text-status-warning" : "border-status-error-border bg-status-error-bg text-status-error"}>
                {wsState}
              </Badge>
            </div>
            <CardContent className="space-y-4 pt-4">
              <div className="flex flex-wrap gap-2">
                <Button size="sm" variant="outline" className="gap-1.5" onClick={() => setWsState("connected")}>
                  <Wifi className="size-3.5 text-status-success" /> Connected
                </Button>
                <Button size="sm" variant="outline" className="gap-1.5" onClick={() => setWsState("reconnecting")}>
                  <RefreshCw className="size-3.5 text-status-warning" /> Reconnecting
                </Button>
                <Button size="sm" variant="outline" className="gap-1.5" onClick={() => setWsState("disconnected")}>
                  <WifiOff className="size-3.5 text-status-error" /> Disconnected
                </Button>
              </div>

              {/* 预览 Banner */}
              <WsStatusBanner wsState={wsState} />

              {/* 预览 StatusBar */}
              <LiveStatusBar
                wsState={wsState}
                lastUpdate="17:18:37"
                subscriptions={wsState === "disconnected" ? 0 : 10}
                updateRate={wsState === "connected" ? 320 : 0}
                staleCount={wsState === "connected" ? 0 : wsState === "reconnecting" ? 2 : 10}
              />
            </CardContent>
          </Card>
        </Section>

        <Separator />

        {/* 订阅失败 Banner */}
        <Section title="2 · Subscription Error Banner">
          <Card className="gap-0">
            <CardContent className="space-y-3 pt-5">
              <Button size="sm" variant="outline" className="gap-1.5"
                onClick={() => setShowSubErr((s) => !s)}>
                <AlertTriangle className="size-3.5" />
                {showSubErr ? "隐藏订阅错误" : "模拟订阅失败"}
              </Button>
              {showSubErr && (
                <SubscriptionErrorBanner
                  count={2}
                  reasons={["topic permission denied", "transport unavailable"]}
                  onRetry={() => { setShowSubErr(false); toast.success("已重新订阅"); }}
                  onDismiss={() => setShowSubErr(false)}
                />
              )}
            </CardContent>
          </Card>
        </Section>

        <Separator />

        {/* GET /data/latest 加载失败 */}
        <Section title="3 · GET /data/latest 加载失败">
          <Card className="gap-0">
            <CardContent className="space-y-3 pt-5">
              <Button size="sm" variant="outline" className="gap-1.5"
                onClick={() => setShowLatestErr((s) => !s)}>
                <ServerCrash className="size-3.5" />
                {showLatestErr ? "恢复正常" : "模拟加载失败"}
              </Button>
              {showLatestErr && (
                <LatestDataErrorState onRetry={() => { setShowLatestErr(false); toast.success("已重试"); }} />
              )}
            </CardContent>
          </Card>
        </Section>

        <Separator />

        {/* Empty 状态 */}
        <Section title="4 · Empty 状态">
          <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
            <Card className="gap-0">
              <div className="px-5 pt-5 text-xs text-muted-foreground">有筛选条件</div>
              <CardContent className="pt-2">
                <LiveEmptyState isFiltering canConfig onReset={() => toast.message("重置筛选")} />
              </CardContent>
            </Card>
            <Card className="gap-0">
              <div className="px-5 pt-5 text-xs text-muted-foreground">无筛选 + Viewer 无配置权限</div>
              <CardContent className="pt-2">
                <LiveEmptyState isFiltering={false} canConfig={false} onReset={() => {}} />
              </CardContent>
            </Card>
          </div>
        </Section>

        <Separator />

        {/* 立即拉取四态 */}
        <Section title="5 · 立即拉取 ReadNowResultPanel（四态）">
          <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
            <Card className="gap-0">
              <div className="px-5 pt-5 text-xs text-muted-foreground">有控制权限 (Operator/Admin)</div>
              <CardContent className="pt-2">
                <ReadNowResultPanel canControl datapointId="dp1" />
              </CardContent>
            </Card>
            <Card className="gap-0">
              <div className="px-5 pt-5 text-xs text-muted-foreground">立即拉取失败 (dp9 = offline)</div>
              <CardContent className="pt-2">
                <ReadNowResultPanel canControl datapointId="dp9" />
              </CardContent>
            </Card>
            <Card className="gap-0">
              <div className="px-5 pt-5 text-xs text-muted-foreground">Viewer 无控制权限</div>
              <CardContent className="pt-2">
                <ReadNowResultPanel canControl={false} datapointId="dp1" />
              </CardContent>
            </Card>
          </div>
        </Section>

        <Separator />

        {/* 原子组件色板 */}
        <Section title="6 · 状态/质量原子组件色板">
          <Card className="gap-0">
            <CardContent className="flex flex-wrap gap-3 pt-5">
              <div className="space-y-2">
                <p className="text-xs text-muted-foreground">Quality</p>
                <div className="flex gap-2">
                  <QualityBadge quality="Good" />
                  <QualityBadge quality="Uncertain" />
                  <QualityBadge quality="Bad" />
                </div>
              </div>
              <div className="space-y-2">
                <p className="text-xs text-muted-foreground">Status</p>
                <div className="flex flex-wrap gap-2">
                  <DpStatusBadge status="normal" />
                  <DpStatusBadge status="alarm" />
                  <DpStatusBadge status="error" />
                  <DpStatusBadge status="offline" />
                  <DpStatusBadge status="stale" />
                </div>
              </div>
              <div className="space-y-2">
                <p className="text-xs text-muted-foreground">StaleDataTag</p>
                <div className="flex gap-2 items-center font-mono text-xs">
                  <span>17:18:37</span>
                  <StaleDataTag ageSeconds={56} />
                </div>
              </div>
            </CardContent>
          </Card>
        </Section>
      </div>
    </>
  );
}
