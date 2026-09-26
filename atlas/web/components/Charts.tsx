"use client";
// Minimal SVG charts. Each chart also has an accessible table in the page that
// uses it, so the numbers are never only visual.
import { fmtNs } from "@/lib/format";

const PALETTE = ["#60a5fa", "#2dd4bf", "#fbbf24", "#a78bfa", "#f472b6", "#34d399", "#f87171", "#94a3b8"];
export const colorFor = (key: string, i: number) => (key.includes("lpq") ? ["#a78bfa", "#c4b5fd", "#8b5cf6"][i % 3] : PALETTE[i % PALETTE.length]);

/** Per-query medians for each configuration on a log time axis, with each configuration's median marked. */
export function StripPlot({ series, width = 760 }: { series: { key: string; values: number[] }[]; width?: number }) {
  const all = series.flatMap((s) => s.values).filter((v) => v > 0);
  if (all.length === 0) return null;
  const lo = Math.log10(Math.min(...all)) - 0.05;
  const hi = Math.log10(Math.max(...all)) + 0.05;
  const left = 190;
  const right = 20;
  const rowH = 30;
  const h = series.length * rowH + 34;
  const x = (v: number) => left + ((Math.log10(v) - lo) / (hi - lo)) * (width - left - right);
  const ticks: number[] = [];
  for (let e = Math.floor(lo); e <= Math.ceil(hi); e++) for (const m of [1, 2, 5]) {
    const v = m * 10 ** e;
    if (Math.log10(v) >= lo && Math.log10(v) <= hi) ticks.push(v);
  }
  return (
    <svg className="chart" width="100%" viewBox={`0 0 ${width} ${h}`} role="img" aria-label="Per-query median runtime by configuration (log scale)">
      {ticks.map((t) => (
        <g key={t}>
          <line className="gridline" x1={x(t)} x2={x(t)} y1={6} y2={h - 26} />
          <text x={x(t)} y={h - 10} textAnchor="middle">{fmtNs(t)}</text>
        </g>
      ))}
      {series.map((s, i) => {
        const y = 18 + i * rowH;
        const sorted = [...s.values].sort((a, b) => a - b);
        const med = sorted.length ? sorted[Math.floor((sorted.length - 1) / 2)] * 0.5 + sorted[Math.ceil((sorted.length - 1) / 2)] * 0.5 : 0;
        const c = colorFor(s.key, i);
        return (
          <g key={s.key}>
            <text x={left - 8} y={y + 4} textAnchor="end" style={{ fill: "var(--text)" }}>{s.key.replace("dijkstra/", "").replace("/exact", "")}</text>
            {s.values.map((v, j) => (
              <circle key={j} cx={x(v)} cy={y + ((j * 7) % 11) - 5} r={2.2} fill={c} opacity={0.55} />
            ))}
            {med > 0 ? <line x1={x(med)} x2={x(med)} y1={y - 10} y2={y + 10} stroke="#fff" strokeWidth={2} /> : null}
          </g>
        );
      })}
    </svg>
  );
}

/** Horizontal bars for one non-negative quantity per configuration. */
export function Bars({ items, format, width = 760, label }: { items: { key: string; value: number }[]; format: (v: number) => string; width?: number; label: string }) {
  if (items.length === 0) return null;
  const max = Math.max(...items.map((i) => i.value), 1);
  const left = 190;
  const rowH = 24;
  const h = items.length * rowH + 8;
  return (
    <svg className="chart" width="100%" viewBox={`0 0 ${width} ${h}`} role="img" aria-label={label}>
      {items.map((it, i) => {
        const y = 4 + i * rowH;
        const w = ((width - left - 110) * it.value) / max;
        return (
          <g key={it.key}>
            <text x={left - 8} y={y + 14} textAnchor="end" style={{ fill: "var(--text)" }}>{it.key.replace("dijkstra/", "").replace("/exact", "")}</text>
            <rect x={left} y={y + 3} width={Math.max(1, w)} height={rowH - 8} fill={colorFor(it.key, i)} rx={2} />
            <text x={left + w + 6} y={y + 14}>{format(it.value)}</text>
          </g>
        );
      })}
    </svg>
  );
}
