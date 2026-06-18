import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { Search, FileDown, LineChart as LineChartIcon, Loader2 } from "lucide-react";
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer,
} from "recharts";
import { apiGet } from "../api";
import { PageHeader } from "../components/PageHeader";
import { Button } from "../components/ui/button";
import { Input } from "../components/ui/input";
import { Badge } from "../components/ui/badge";
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "../components/ui/table";

interface DpRow { id: string; transport_id: string; reg_table: string; addr: string }
interface HistRow { ts: number; value_num: number | null; value_text: string; quality: string }
interface HistResp { total: number; rows: HistRow[] }

const COLORS = ["#2563eb", "#16a34a", "#d97706", "#db2777", "#7c3aed"];
const QUALITY_CLS: Record<string, string> = {
  Good: "bg-emerald-50 text-emerald-700 border-emerald-200",
  Ok: "bg-emerald-50 text-emerald-700 border-emerald-200",
  Bad: "bg-red-50 text-red-700 border-red-200",
};

function toLocalInput(ms: number) {
  const d = new Date(ms - new Date().getTimezoneOffset() * 60000);
  return d.toISOString().slice(0, 16);
}
const hhmmss = (ms: number) => new Date(ms).toLocaleTimeString("zh-CN", { hour12: false });

export function History() {
  const [points, setPoints] = useState<DpRow[]>([]);
  const [selected, setSelected] = useState<string[]>([]);
  const [from, setFrom] = useState(() => toLocalInput(Date.now() - 3600_000));
  const [to, setTo] = useState(() => toLocalInput(Date.now()));
  const [queried, setQueried] = useState(false);
  const [loading, setLoading] = useState(false);
  // chart: 行 = { ts, [id]: value }；table: 扁平
  const [chart, setChart] = useState<Record<string, number | string>[]>([]);
  const [tableRows, setTableRows] = useState<{ ts: number; id: string; value: string; quality: string }[]>([]);

  useEffect(() => {
    apiGet<DpRow[]>("/datapoints")
      .then((dps) => { setPoints(dps); setSelected(dps.slice(0, 2).map((d) => d.id)); })
      .catch(() => toast.error("加载采集点失败"));
  }, []);

  async function runQuery() {
    if (selected.length === 0) { toast.error("请先选择至少一个点位"); return; }
    setLoading(true);
    try {
      const fromMs = new Date(from).getTime();
      const toMs = new Date(to).getTime();
      const results = await Promise.all(
        selected.map((id) => apiGet<HistResp>(`/data/history?id=${encodeURIComponent(id)}&from=${fromMs}&to=${toMs}&size=500`)
          .then((r) => ({ id, rows: r.rows })).catch(() => ({ id, rows: [] as HistRow[] }))),
      );
      // 合并图表（ts 对齐）
      const byTs = new Map<number, Record<string, number | string>>();
      const flat: { ts: number; id: string; value: string; quality: string }[] = [];
      for (const { id, rows } of results) {
        for (const r of rows) {
          if (!byTs.has(r.ts)) byTs.set(r.ts, { ts: r.ts, time: hhmmss(r.ts) });
          byTs.get(r.ts)![id] = r.value_num ?? (Number(r.value_text) || 0);
          flat.push({ ts: r.ts, id, value: r.value_text || String(r.value_num ?? "-"), quality: r.quality });
        }
      }
      setChart([...byTs.values()].sort((a, b) => (a.ts as number) - (b.ts as number)));
      setTableRows(flat.sort((a, b) => b.ts - a.ts).slice(0, 200));
      setQueried(true);
      const n = results.reduce((s, r) => s + r.rows.length, 0);
      if (n === 0) toast.message("该时间范围暂无历史样本（后端采样每 2s 写入）");
    } catch (e) {
      toast.error(e instanceof Error ? e.message : "查询失败");
    } finally { setLoading(false); }
  }

  function toggle(id: string) {
    setSelected((p) => (p.includes(id) ? p.filter((x) => x !== id) : [...p, id]));
  }

  const csv = useMemo(() => () => {
    const lines = ["time,id,value,quality", ...tableRows.map((r) => `${hhmmss(r.ts)},${r.id},${r.value},${r.quality}`)];
    const blob = new Blob([lines.join("\n")], { type: "text/csv" });
    const a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = "history.csv"; a.click();
  }, [tableRows]);

  return (
    <>
      <PageHeader
        title="历史数据" en="History"
        description="查询点位历史数据（后端 /data/history），支持多点位趋势对比与导出。"
        actions={
          <>
            <Button size="sm" className="gap-1.5" onClick={runQuery} disabled={loading}>{loading ? <Loader2 className="size-3.5 animate-spin" /> : <Search className="size-3.5" />}查询</Button>
            <Button variant="outline" size="sm" className="gap-1.5" disabled={!queried || tableRows.length === 0} onClick={csv}><FileDown className="size-3.5" />导出 CSV</Button>
          </>
        }
      />

      <div className="space-y-4 p-6">
        <div className="space-y-3 rounded-md border border-border bg-card p-4">
          <div className="flex flex-wrap items-center gap-2">
            <span className="text-sm text-muted-foreground">时间范围</span>
            <Input type="datetime-local" className="h-8 w-52" value={from} onChange={(e) => setFrom(e.target.value)} />
            <span className="text-muted-foreground">~</span>
            <Input type="datetime-local" className="h-8 w-52" value={to} onChange={(e) => setTo(e.target.value)} />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <span className="text-sm text-muted-foreground">点位</span>
            {points.length === 0 && <span className="text-xs text-muted-foreground">（无采集点，请先到 Datapoints 配置）</span>}
            {points.map((p) => (
              <button key={p.id} type="button" onClick={() => toggle(p.id)}
                className={`rounded-full border px-3 py-1 text-xs transition ${selected.includes(p.id) ? "border-transparent bg-primary text-primary-foreground" : "border-border bg-background text-muted-foreground hover:bg-muted"}`}>
                {p.id}
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
            <div className="rounded-md border border-border bg-card p-4">
              <div className="mb-2 text-sm font-medium">趋势图</div>
              <div className="h-80">
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={chart} margin={{ top: 8, right: 16, bottom: 0, left: 0 }}>
                    <CartesianGrid strokeDasharray="3 3" stroke="#eef2f6" />
                    <XAxis dataKey="time" tick={{ fontSize: 11 }} minTickGap={24} />
                    <YAxis tick={{ fontSize: 11 }} />
                    <Tooltip />
                    <Legend />
                    {selected.map((id, i) => (
                      <Line key={id} type="monotone" dataKey={id} name={id} stroke={COLORS[i % COLORS.length]} dot={false} strokeWidth={1.8} connectNulls />
                    ))}
                  </LineChart>
                </ResponsiveContainer>
              </div>
            </div>

            <div className="rounded-md border border-border bg-card">
              <Table>
                <TableHeader>
                  <TableRow><TableHead>时间</TableHead><TableHead>点位</TableHead><TableHead>值</TableHead><TableHead>质量</TableHead></TableRow>
                </TableHeader>
                <TableBody>
                  {tableRows.map((r, i) => (
                    <TableRow key={i}>
                      <TableCell className="font-mono text-xs">{hhmmss(r.ts)}</TableCell>
                      <TableCell className="font-medium">{r.id}</TableCell>
                      <TableCell>{r.value}</TableCell>
                      <TableCell><Badge variant="outline" className={QUALITY_CLS[r.quality] ?? "bg-amber-50 text-amber-700 border-amber-200"}>{r.quality}</Badge></TableCell>
                    </TableRow>
                  ))}
                  {tableRows.length === 0 && <TableRow><TableCell colSpan={4} className="h-24 text-center text-muted-foreground">该范围无样本</TableCell></TableRow>}
                </TableBody>
              </Table>
            </div>
          </>
        )}
      </div>
    </>
  );
}
