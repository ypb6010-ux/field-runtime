import { useMemo, useState } from "react";
import { toast } from "sonner";
import { Search, FileDown, FileSpreadsheet, LineChart as LineChartIcon } from "lucide-react";
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer,
} from "recharts";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Badge } from "../components/ui/badge";
import {
  Select, SelectContent, SelectItem, SelectTrigger, SelectValue,
} from "../components/ui/select";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";

const POINTS = [
  { id: "boiler_temp", name: "锅炉水温", unit: "℃", transport: "PLC-1 产线A", addr: "HR0", color: "#2563eb" },
  { id: "pump_press", name: "主泵压力", unit: "bar", transport: "PLC-1 产线A", addr: "HR1", color: "#16a34a" },
  { id: "motor_rpm", name: "电机转速", unit: "rpm", transport: "OPC-UA 产线B", addr: "Sim_1", color: "#d97706" },
];

const QUALITY: Record<string, { label: string; cls: string }> = {
  Good: { label: "Good", cls: "bg-emerald-50 text-emerald-700 border-emerald-200" },
  Bad: { label: "Bad", cls: "bg-red-50 text-red-700 border-red-200" },
  Uncertain: { label: "Uncertain", cls: "bg-amber-50 text-amber-700 border-amber-200" },
};

function genSeries(points: string[]) {
  const out: Record<string, number | string>[] = [];
  for (let i = 0; i < 48; i++) {
    const t = new Date(Date.now() - (48 - i) * 60_000);
    const row: Record<string, number | string> = { time: `${String(t.getHours()).padStart(2, "0")}:${String(t.getMinutes()).padStart(2, "0")}` };
    if (points.includes("boiler_temp")) row.boiler_temp = +(25 + 8 * Math.sin(i / 6) + Math.random()).toFixed(1);
    if (points.includes("pump_press")) row.pump_press = +(5 + 0.6 * Math.sin(i / 4 + 1) + Math.random() * 0.2).toFixed(2);
    if (points.includes("motor_rpm")) row.motor_rpm = Math.round(1450 + 120 * Math.sin(i / 5) + Math.random() * 30);
    out.push(row);
  }
  return out;
}

export function History() {
  const [selected, setSelected] = useState<string[]>(["boiler_temp", "pump_press"]);
  const [agg, setAgg] = useState("raw");
  const [interval, setIntervalVal] = useState("1m");
  const [quality, setQuality] = useState("all");
  const [queried, setQueried] = useState(false);
  const [data, setData] = useState<Record<string, number | string>[]>([]);

  const tableRows = useMemo(() => {
    if (!queried) return [];
    const rows: { time: string; pid: string }[] = [];
    for (const r of data.slice(-20).reverse()) {
      for (const pid of selected) if (r[pid] !== undefined) rows.push({ time: r.time as string, pid });
    }
    return rows.map((r, i) => ({ ...r, q: i % 11 === 0 ? "Uncertain" : "Good", val: data.find((d) => d.time === r.time)?.[r.pid] }));
  }, [queried, data, selected]);

  function runQuery() {
    if (selected.length === 0) { toast.error("请先选择至少一个点位"); return; }
    setData(genSeries(selected));
    setQueried(true);
    toast.success(`已查询 ${selected.length} 个点位`);
  }

  function togglePoint(id: string) {
    setSelected((p) => (p.includes(id) ? p.filter((x) => x !== id) : [...p, id]));
  }

  return (
    <>
      <PageHeader
        title="历史数据"
        en="History"
        description="查询点位历史数据，支持趋势对比与导出。"
        actions={
          <>
            <Button size="sm" className="gap-1.5" onClick={runQuery}><Search className="size-3.5" />查询</Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!queried} onClick={() => toast.message("导出 CSV（演示）")}><FileDown className="size-3.5" />导出 CSV</Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!queried} onClick={() => toast.message("导出 Excel（演示）")}><FileSpreadsheet className="size-3.5" />导出 Excel</Button>
          </>
        }
      />

      <div className="space-y-4 p-6">
        {/* Query Panel */}
        <div className="space-y-3 rounded-md border border-border bg-card p-4">
          <div className="flex flex-wrap items-center gap-2">
            <span className="text-sm text-muted-foreground">时间范围</span>
            <Input type="datetime-local" className="h-8 w-52" defaultValue="2026-06-18T09:00" />
            <span className="text-muted-foreground">~</span>
            <Input type="datetime-local" className="h-8 w-52" defaultValue="2026-06-18T10:30" />
            <Select value={agg} onValueChange={setAgg}>
              <SelectTrigger className="h-8 w-32"><SelectValue placeholder="聚合" /></SelectTrigger>
              <SelectContent><SelectItem value="raw">raw</SelectItem><SelectItem value="avg">avg</SelectItem><SelectItem value="min">min</SelectItem><SelectItem value="max">max</SelectItem></SelectContent>
            </Select>
            <Select value={interval} onValueChange={setIntervalVal}>
              <SelectTrigger className="h-8 w-28"><SelectValue placeholder="间隔" /></SelectTrigger>
              <SelectContent><SelectItem value="1s">1s</SelectItem><SelectItem value="10s">10s</SelectItem><SelectItem value="1m">1m</SelectItem><SelectItem value="5m">5m</SelectItem></SelectContent>
            </Select>
            <Select value={quality} onValueChange={setQuality}>
              <SelectTrigger className="h-8 w-32"><SelectValue placeholder="质量" /></SelectTrigger>
              <SelectContent><SelectItem value="all">全部质量</SelectItem><SelectItem value="Good">Good</SelectItem><SelectItem value="Bad">Bad</SelectItem><SelectItem value="Uncertain">Uncertain</SelectItem></SelectContent>
            </Select>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <span className="text-sm text-muted-foreground">点位</span>
            {POINTS.map((p) => (
              <button key={p.id} type="button" onClick={() => togglePoint(p.id)}
                className={`rounded-full border px-3 py-1 text-xs transition ${selected.includes(p.id) ? "border-transparent bg-primary text-primary-foreground" : "border-border bg-background text-muted-foreground hover:bg-muted"}`}>
                {p.name} <span className="opacity-70">{p.unit}</span>
              </button>
            ))}
          </div>
        </div>

        {!queried ? (
          <div className="flex flex-col items-center justify-center gap-2 rounded-md border border-dashed border-border bg-card py-20 text-center">
            <LineChartIcon className="size-8 text-muted-foreground/50" />
            <div className="text-sm font-medium">尚未查询</div>
            <div className="text-sm text-muted-foreground">请选择时间范围和点位后点击查询。</div>
          </div>
        ) : (
          <>
            {/* Trend Chart */}
            <div className="rounded-md border border-border bg-card p-4">
              <div className="mb-2 text-sm font-medium">趋势图</div>
              <div className="h-80">
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={data} margin={{ top: 8, right: 16, bottom: 0, left: 0 }}>
                    <CartesianGrid strokeDasharray="3 3" stroke="#eef2f6" />
                    <XAxis dataKey="time" tick={{ fontSize: 11 }} minTickGap={24} />
                    <YAxis tick={{ fontSize: 11 }} />
                    <Tooltip />
                    <Legend />
                    {POINTS.filter((p) => selected.includes(p.id)).map((p) => (
                      <Line key={p.id} type="monotone" dataKey={p.id} name={`${p.name}(${p.unit})`} stroke={p.color} dot={false} strokeWidth={1.8} />
                    ))}
                  </LineChart>
                </ResponsiveContainer>
              </div>
            </div>

            {/* Table */}
            <div className="rounded-md border border-border bg-card">
              <Table>
                <TableHeader>
                  <TableRow>
                    <TableHead>时间</TableHead><TableHead>点位名称</TableHead><TableHead>值</TableHead>
                    <TableHead>单位</TableHead><TableHead>质量</TableHead><TableHead>Transport</TableHead><TableHead>地址</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {tableRows.map((r, i) => {
                    const p = POINTS.find((x) => x.id === r.pid)!;
                    return (
                      <TableRow key={i}>
                        <TableCell className="font-mono text-xs">{r.time}</TableCell>
                        <TableCell className="font-medium">{p.name}</TableCell>
                        <TableCell>{String(r.val ?? "-")}</TableCell>
                        <TableCell>{p.unit}</TableCell>
                        <TableCell><Badge variant="outline" className={QUALITY[r.q].cls}>{QUALITY[r.q].label}</Badge></TableCell>
                        <TableCell className="text-muted-foreground">{p.transport}</TableCell>
                        <TableCell className="font-mono text-xs">{p.addr}</TableCell>
                      </TableRow>
                    );
                  })}
                </TableBody>
              </Table>
            </div>
          </>
        )}
      </div>
    </>
  );
}
