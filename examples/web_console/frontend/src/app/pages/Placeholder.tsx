import { Construction } from "lucide-react";
import type { PageKey } from "../types";
import { getNavItem } from "../nav";
import { PageHeader } from "../components/PageHeader";
import { Card } from "../components/ui/card";

/** 第一轮骨架：除 Dashboard 外，各页以统一占位卡呈现，标明后续将承载的核心内容。 */
const SCOPE: Partial<Record<PageKey, string[]>> = {
  protocols: ["协议连接列表（表格）", "新建 / 编辑连接（右侧抽屉）", "连接测试与状态徽标", "Modbus / OPC UA / MQTT 等驱动配置"],
  datapoints: ["采集点列表（可筛选 / 批量）", "点位映射与数据类型", "导入 / 导出 CSV", "采集点详情抽屉"],
  polling: ["轮询任务列表", "采集周期 / 超时 / 重试策略", "任务启停与运行状态", "任务编辑抽屉"],
  conversion: ["转换规则列表", "源 → 目标字段映射", "表达式 / 脚本编辑器", "规则校验与试运行"],
  live: ["实时数据网格（WebSocket）", "连接状态灯墙", "单点实时趋势", "暂停 / 订阅过滤"],
  history: ["时间范围查询", "ECharts 趋势对比", "聚合 / 降采样", "数据导出"],
  config: ["Draft vs Active 配置 Diff", "校验 → 发布 → 回滚流程", "未生效变更清单", "发布历史时间线"],
  settings: ["网关基础参数", "存储 / 保留策略", "告警通道", "时区与本地化"],
  apidocs: ["REST / WebSocket 接口说明", "鉴权与 Token", "在线调试", "示例代码"],
  logs: ["事件 / 审计日志表", "级别 / 模块 / 操作人筛选", "日志详情抽屉", "审计导出"],
  users: ["用户列表与状态", "角色分配（Viewer/Operator/Admin）", "权限矩阵", "邀请 / 停用"],
};

export function Placeholder({ page }: { page: PageKey }) {
  const item = getNavItem(page);
  if (!item) return null;
  const scope = SCOPE[page] ?? [];

  return (
    <>
      <PageHeader title={item.label} en={item.en} description="第一轮骨架页，下一轮细化交互与数据。" />
      <div className="p-6">
        <Card className="flex flex-col items-center justify-center gap-4 border-dashed py-16 text-center">
          <div className="flex size-12 items-center justify-center rounded-full bg-muted">
            <Construction className="size-6 text-muted-foreground" />
          </div>
          <div>
            <div className="text-base">{item.label} · {item.en}</div>
            <p className="mt-1 max-w-md text-sm text-muted-foreground">
              此页面已纳入信息架构，本轮仅占位。规划将承载以下核心内容：
            </p>
          </div>
          <ul className="grid w-full max-w-lg grid-cols-1 gap-2 text-left sm:grid-cols-2">
            {scope.map((s) => (
              <li
                key={s}
                className="flex items-start gap-2 rounded-md border border-border bg-muted/30 px-3 py-2 text-sm"
              >
                <span className="mt-1.5 size-1.5 shrink-0 rounded-full bg-primary" />
                {s}
              </li>
            ))}
          </ul>
        </Card>
      </div>
    </>
  );
}
