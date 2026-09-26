"use client";
// A stored benchmark result, summarised by the API from lpq_benchmark's raw
// records. Nothing is re-measured here and no configuration is crowned: the
// fastest is named by its measured median, and paired statistics report effect
// size next to significance (spec sections 15, 32, 34).
import { Bars, StripPlot } from "./Charts";
import type { ResultSummary } from "@/lib/types";
import { fmtInt, fmtNs, fmtNum, fmtPct } from "@/lib/format";

export function ResultView({ s }: { s: ResultSummary }) {
  const measured = s.configurations.filter((c) => c.supported);
  const unsupported = s.configurations.filter((c) => !c.supported);
  const env = s.environment ?? {};
  return (
    <div className="stack">
      <div className="row small muted">
        <span className="badge">{s.experiment_id}</span>
        <span>{s.records} records · {s.graph_nodes ? `${fmtInt(s.graph_nodes)} nodes` : ""} · generated {s.generated_at}</span>
        <span>· commit {String(env.git_commit ?? "?").slice(0, 10)}{env.git_dirty ? " (dirty)" : ""} · {String(env.cpu_model ?? "")}</span>
      </div>
      {s.fastest_by_median ? (
        <div className="notice info small">
          Lowest median of per-query medians: <strong>{s.fastest_by_median}</strong>. This is a measurement on this query set and machine, not a verdict.
        </div>
      ) : null}

      <div className="panel">
        <h2>Runtime per query</h2>
        <p className="small muted">Each dot is one query&apos;s median over its repetitions; the white tick is the configuration&apos;s median. Log scale.</p>
        <StripPlot series={measured.map((c) => ({ key: c.key, values: c.per_query_median_ns ?? [] }))} />
      </div>

      <div className="grid cols-2">
        <div className="panel">
          <h2>Queue comparisons (total)</h2>
          <p className="small muted">Includes the learned queue&apos;s verification comparisons.</p>
          <Bars label="Total comparisons" items={measured.map((c) => ({ key: c.key, value: c.total_comparisons ?? 0 }))} format={(v) => fmtInt(v)} />
        </div>
        <div className="panel">
          <h2>Nodes settled (total)</h2>
          <Bars label="Total settled nodes" items={measured.map((c) => ({ key: c.key, value: c.total_settled ?? 0 }))} format={(v) => fmtInt(v)} />
        </div>
      </div>

      <div className="panel scroll-x">
        <h2>Configurations</h2>
        <table className="data">
          <thead>
            <tr>
              <th>Configuration</th><th className="num">Queries</th><th className="num">Median</th><th className="num">MAD (across queries)</th>
              <th className="num">Noise (within)</th><th className="num">Comparisons</th><th className="num">Pops</th><th className="num">Settled</th>
              <th className="num">Exact</th><th className="num">Max ratio</th><th>Pinned</th>
            </tr>
          </thead>
          <tbody>
            {measured.map((c) => (
              <tr key={c.key}>
                <td className="mono">{c.key}</td>
                <td className="num">{c.queries}</td>
                <td className="num">{fmtNs(c.median_of_medians_ns)}</td>
                <td className="num">{fmtNs(c.mad_of_medians_ns)}</td>
                <td className="num">{fmtPct(c.within_query_noise)}</td>
                <td className="num">{fmtInt(c.total_comparisons)}</td>
                <td className="num">{fmtInt(c.total_pops)}</td>
                <td className="num">{fmtInt(c.total_settled)}</td>
                <td className="num">{c.exact_matches}/{c.queries}</td>
                <td className="num">{fmtNum(c.max_approximation_ratio ?? null, 6)}</td>
                <td>{c.all_pinned ? "yes" : "NO"}{c.noisy_queries ? ` · ${c.noisy_queries} noisy` : ""}</td>
              </tr>
            ))}
            {unsupported.map((c) => (
              <tr key={c.key}>
                <td className="mono">{c.key}</td>
                <td colSpan={10} className="small" style={{ color: "var(--warn)" }}>not measured: {c.unsupported_reason}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="panel scroll-x">
        <h2>Paired comparison against {s.baseline ?? "baseline"}</h2>
        <p className="small muted">
          Same queries, paired. Ratio &gt; 1: the configuration is slower than the baseline on the median query. p-values from a seeded paired
          permutation test, Holm-corrected across this family. Significance is not effect size: read the ratio and the rank-biserial first.
        </p>
        {s.paired_comparisons.length === 0 ? <p className="small dim">Needs at least two measured configurations on the same queries.</p> : (
          <table className="data">
            <thead>
              <tr><th>Configuration</th><th className="num">Median ratio</th><th className="num">Median Δ</th><th className="num">95% CI of Δ</th>
                <th className="num">Rank-biserial</th><th className="num">Baseline faster on</th><th className="num">p (Holm)</th></tr>
            </thead>
            <tbody>
              {s.paired_comparisons.map((p) => (
                <tr key={p.config}>
                  <td className="mono">{p.config}</td>
                  <td className="num">{fmtNum(p.median_ratio_vs_baseline, 3)}×</td>
                  <td className="num">{fmtNs(Math.abs(p.median_difference_ns))} {p.median_difference_ns > 0 ? "slower" : "faster"}</td>
                  <td className="num">[{fmtNs(p.difference_ci_ns[0])}, {fmtNs(p.difference_ci_ns[1])}]</td>
                  <td className="num">{fmtNum(p.rank_biserial, 3)}</td>
                  <td className="num">{fmtPct(p.fraction_baseline_faster)} of queries</td>
                  <td className="num">{p.adjusted_p_value !== null ? p.adjusted_p_value.toExponential(2) : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      <p className="small dim">Hardware counters: {String((s.hardware_counters as Record<string, unknown> | null)?.unavailable_reason ?? "not collected")}</p>
    </div>
  );
}
