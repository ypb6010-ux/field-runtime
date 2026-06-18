import { Check, Minus, Home } from "lucide-react";
import type { Role } from "../types";
import { ROLE_LABELS } from "../types";
import { NAV_GROUPS, ROLE_DEFAULT_PAGE, getNavItem } from "../nav";
import { PageHeader } from "../components/PageHeader";
import { Card, CardContent, CardTitle } from "../components/ui/card";
import { Badge } from "../components/ui/badge";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "../components/ui/table";

const ROLES: Role[] = ["viewer", "operator", "admin"];

// 业务导航项（排除“设计说明”分组）
const BIZ_ITEMS = NAV_GROUPS.filter((g) => g.id !== "spec").flatMap((g) => g.items);

const ROLE_ACCENT: Record<Role, string> = {
  viewer: "border-t-status-disabled",
  operator: "border-t-status-warning",
  admin: "border-t-primary",
};

export function RoleNavigation() {
  return (
    <>
      <PageHeader
        title="角色导航对比"
        en="Role Navigation"
        description="三种角色登录后可见的导航与默认落地页（设计说明，非业务页面）。"
      />
      <div className="space-y-6 p-6">
        {/* 三列卡片 */}
        <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
          {ROLES.map((role) => {
            const items = BIZ_ITEMS.filter((i) => i.roles.includes(role));
            const landing = getNavItem(ROLE_DEFAULT_PAGE[role]);
            return (
              <Card key={role} className={`gap-0 border-t-2 ${ROLE_ACCENT[role]}`}>
                <div className="flex items-center justify-between px-6 pt-6">
                  <CardTitle>{ROLE_LABELS[role]}</CardTitle>
                  <Badge variant="secondary">{items.length} 项</Badge>
                </div>
                <div className="px-6 pb-2 pt-1">
                  <span className="inline-flex items-center gap-1 rounded bg-secondary px-2 py-0.5 text-xs text-primary">
                    <Home className="size-3" />
                    默认落地：{landing?.label} {landing?.en}
                  </span>
                </div>
                <CardContent className="pt-2">
                  <ul className="space-y-0.5">
                    {items.map((item) => {
                      const Icon = item.icon;
                      return (
                        <li
                          key={item.key}
                          className="flex items-center gap-2.5 rounded-md px-2 py-1.5 text-sm hover:bg-muted/50"
                        >
                          <Icon className="size-4 text-muted-foreground" />
                          <span>{item.label}</span>
                          <span className="text-xs text-muted-foreground">{item.en}</span>
                        </li>
                      );
                    })}
                  </ul>
                </CardContent>
              </Card>
            );
          })}
        </div>

        {/* 对比表 */}
        <Card className="gap-0">
          <div className="px-6 pt-6">
            <CardTitle>权限矩阵 Permission Matrix</CardTitle>
          </div>
          <CardContent className="pt-4">
            <div className="overflow-hidden rounded-md border border-border">
              <Table>
                <TableHeader>
                  <TableRow className="bg-muted/50">
                    <TableHead className="w-[40%]">页面</TableHead>
                    {ROLES.map((r) => (
                      <TableHead key={r} className="text-center">
                        {ROLE_LABELS[r].split(" ")[0]}
                      </TableHead>
                    ))}
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {BIZ_ITEMS.map((item) => {
                    const Icon = item.icon;
                    return (
                      <TableRow key={item.key}>
                        <TableCell>
                          <span className="flex items-center gap-2">
                            <Icon className="size-4 text-muted-foreground" />
                            {item.label}
                            <span className="text-xs text-muted-foreground">{item.en}</span>
                          </span>
                        </TableCell>
                        {ROLES.map((r) => (
                          <TableCell key={r} className="text-center">
                            {item.roles.includes(r) ? (
                              <Check className="mx-auto size-4 text-status-success" />
                            ) : (
                              <Minus className="mx-auto size-4 text-status-disabled" />
                            )}
                          </TableCell>
                        ))}
                      </TableRow>
                    );
                  })}
                </TableBody>
              </Table>
            </div>
          </CardContent>
        </Card>
      </div>
    </>
  );
}
