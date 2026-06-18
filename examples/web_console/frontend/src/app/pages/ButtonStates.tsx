import type { ReactNode } from "react";
import { toast } from "sonner";
import { ShieldAlert } from "lucide-react";
import { PageHeader } from "../components/PageHeader";
import { PermissionButton } from "../components/PermissionButton";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import { Button } from "../components/ui/button";
import { Alert, AlertDescription, AlertTitle } from "../components/ui/alert";

function Demo({
  title,
  description,
  children,
}: {
  title: string;
  description: string;
  children: ReactNode;
}) {
  return (
    <Card className="gap-0">
      <div className="px-6 pt-6">
        <CardTitle>{title}</CardTitle>
      </div>
      <CardContent className="flex flex-col gap-4 pt-3">
        <p className="text-sm text-muted-foreground">{description}</p>
        <div className="flex flex-wrap items-center gap-3">{children}</div>
      </CardContent>
    </Card>
  );
}

export function ButtonStates() {
  return (
    <>
      <PageHeader
        title="按钮权限状态"
        en="Button Permission States"
        description="按钮级权限控制范式（设计说明，非业务页面）。"
      />
      <div className="space-y-4 p-6">
        <Alert className="border-primary/30 bg-secondary">
          <ShieldAlert className="size-4 text-primary" />
          <AlertTitle>前端只做体验，后端才是权限权威</AlertTitle>
          <AlertDescription>
            菜单过滤与按钮禁用仅用于提升体验；任何操作后端都会再次校验，前端不可绕过。
          </AlertDescription>
        </Alert>

        <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
          <Demo title="1. 有权限" description="正常可点击。">
            <PermissionButton allowed onAction={() => toast.success("已发布配置")}>
              发布配置
            </PermissionButton>
            <PermissionButton allowed onAction={() => toast.success("已保存")} size="sm">
              保存草稿
            </PermissionButton>
          </Demo>

          <Demo
            title="2. 无权限但需展示"
            description="按钮 disabled，hover 显示提示：无权限，请联系管理员。"
          >
            <PermissionButton allowed={false} onAction={() => {}}>
              发布配置
            </PermissionButton>
            <PermissionButton
              allowed={false}
              onAction={() => {}}
              deniedHint="需要 user:write 权限"
            >
              删除用户
            </PermissionButton>
          </Demo>

          <Demo
            title="3. 高风险操作"
            description="危险样式 + 二次确认弹窗，防止误操作。"
          >
            <PermissionButton
              allowed
              danger
              confirm={{
                title: "确认回滚到上一个生效版本？",
                description: "回滚将立即覆盖当前生效配置，采集任务会按新版本重启，请谨慎操作。",
                confirmText: "确认回滚",
              }}
              onAction={() => toast.success("已回滚到 v36")}
            >
              回滚配置
            </PermissionButton>
          </Demo>

          <Demo
            title="4. 后端拒绝"
            description="前端看似有权限，但后端返回 403——提示权限已变化，请刷新。"
          >
            <PermissionButton
              allowed
              onAction={() =>
                toast.error("权限已变化，请刷新权限后重试", {
                  description: "服务器返回 403 Forbidden",
                  action: { label: "刷新权限", onClick: () => toast.message("正在刷新 /auth/me …") },
                })
              }
            >
              执行下发
            </PermissionButton>
            <Button variant="outline" onClick={() => toast.message("正在刷新 /auth/me …")}>
              刷新权限
            </Button>
          </Demo>
        </div>
      </div>
    </>
  );
}
