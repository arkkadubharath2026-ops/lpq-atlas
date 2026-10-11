"use client";
// The Quick Compare results. Every number is read from the API's response; the
// speedup, rank, fastest lane and same-distance verdict are the API's, and are
// only formatted here (lib/compare.ts).
import { Tip } from "@/components/Tip";
import {
  ALGORITHM_TIP,
  ML_TIP,
  QUEUE_TIP,
  laneDef,
} from "@/lib/lanes";
import {
  barPercent,
  correctnessMessage,
  countsRows,
  effectSentence,
  fmtMedian,
  fmtRoundMedians,
  fmtSpread,
  headlineSentences,
  labelResolver,
  measurementNote,
  rankedLanes,
  resolutionText,
  setupText,
  slowestMedian,
  vsVanillaPhrase,
} from "@/lib/compare";
import { fmtInt, fmtNs, fmtPct, queueLabel } from "@/lib/format";
import type { Dataset, LaneComparisonResult, LaneResult } from "@/lib/types";

function laneLabel(l: LaneResult): string {
  return laneDef(l.id)?.label ?? l.label ?? l.id;
}

function Chips({ lane }: { lane: LaneResult }) {
  const ml = lane.family === "learned";
  return (
    <span className="row" style={{ gap: 5 }}>
      <span className="chip">Algorithm <b>{lane.algorithm === "astar" ? "A*" : "Dijkstra"}</b></span>
      <span className="chip">Queue <b>{queueLabel(lane.queue)}</b></span>
      <span className={`chip ${ml ? "ml-on" : ""}`}>ML <b>{ml ? "on" : "off"}</b></span>
    </span>
  );
}

export function LaneBoard({ result, dataset }: { result: LaneComparisonResult; dataset: Dataset | null }) {
  const labelOf = labelResolver(result, (id) => laneDef(id)?.label);
  const lanes = rankedLanes(result);
  const slowest = slowestMedian(result);
  const verdict = correctnessMessage(result, dataset?.osm?.weight_unit);
  const sentences = headlineSentences(result, labelOf);
  const fastestId = result.fastest?.lane_id;
  const tied = new Set(result.fastest?.tied_with ?? []);
  const ok = lanes.filter((l) => l.status === "ok");
  const m = result.measurement;

  return (
    <div className="stack" data-testid="results">
      <div className="summary-box" data-testid="summary">
        {sentences.map((s, i) => (
          <p key={i} className={i === sentences.length - 1 ? "lead" : undefined}>{s}</p>
        ))}
      </div>

      <div className={`notice ${verdict.tone}`} role="status" data-testid="correctness">
        <strong>{verdict.tone === "ok" ? "Correctness check passed. " : verdict.tone === "bad" ? "Correctness check FAILED. " : "Correctness check incomplete. "}</strong>
        {verdict.text}
        <Tip text="Every method is also checked against an independent plain Dijkstra run by the engine. Speed tricks are only allowed if the distance stays exactly the same." />
      </div>

      <section className="panel stack" aria-labelledby="time-h">
        <div>
          <h2 id="time-h">Measured wall-clock time</h2>
          <p className="small muted" style={{ margin: 0 }}>
            Real time the engine spent on one search, measured here just now. Shorter is faster. Ranked fastest first.
          </p>
        </div>
        <ol className="lanes">
          {lanes.map((l) => {
            const phrase = vsVanillaPhrase(l, labelOf);
            const isFastest = l.id === fastestId;
            const isTied = tied.has(l.id);
            const def = laneDef(l.id);
            return (
              <li
                key={l.id}
                className={`lane-row ${isFastest ? "fastest" : ""} ${l.status !== "ok" ? "failed" : ""}`}
                data-testid="lane-row"
                data-lane={l.id}
                data-status={l.status}
                data-fastest={isFastest ? "true" : "false"}
              >
                <div className="lane-top">
                  <span className="lane-rank">{l.rank !== null ? `#${l.rank}` : "–"}</span>
                  <span className="lane-name">
                    {laneLabel(l)}
                    {def ? <Tip text={def.tooltip} /> : null}
                  </span>
                  {isFastest ? (
                    <span className="badge ok" data-testid="fastest-badge">{result.fastest?.clear_win ? "Fastest" : "Fastest (tied)"}</span>
                  ) : isTied ? (
                    <span className="badge ok">Tied for fastest</span>
                  ) : null}
                  {l.matches_reference === false ? <span className="badge bad">Wrong distance</span> : null}
                  <Chips lane={l} />
                  {l.status === "ok" ? (
                    <span className="lane-time" data-testid="lane-median">
                      {fmtMedian(l)} <small>{fmtSpread(l)}</small>
                    </span>
                  ) : null}
                </div>
                {l.status === "ok" && l.timing ? (
                  <>
                    <div className="bar-track" aria-hidden="true">
                      <div className="bar-fill" style={{ width: `${barPercent(l.timing.median_ns, slowest)}%`, background: def?.color ?? "var(--classical)" }} />
                    </div>
                    <div className={`lane-note ${phrase.tone}`} data-testid="lane-vs-vanilla">{phrase.text}</div>
                    {l.timing.round_medians_ns && l.timing.round_medians_ns.length > 1 ? (
                      <div className="small dim" data-testid="lane-rounds">Per-round medians: {fmtRoundMedians(l)}</div>
                    ) : null}
                    {resolutionText(l) ? (
                      <div className="small" style={{ color: l.resolved_to_radix ? "var(--warn)" : "var(--learned)" }} data-testid="lane-resolution">
                        {resolutionText(l)}
                        {l.resolution_reason ? <Tip text={l.resolution_reason} /> : null}
                      </div>
                    ) : null}
                    {setupText(l) ? <div className="small dim" data-testid="lane-setup">{setupText(l)}</div> : null}
                    {l.timing.warnings.length ? (
                      <div className="small" style={{ color: "var(--warn)" }}>{l.timing.warnings.join("; ")}</div>
                    ) : null}
                  </>
                ) : (
                  <div className="lane-note" style={{ color: "var(--bad)", marginTop: 6 }} role="alert" data-testid="lane-error">
                    This lane did not run: {l.error?.message ?? "the engine reported an error"}
                  </div>
                )}
              </li>
            );
          })}
        </ol>
        <p className="small muted" style={{ margin: 0 }} data-testid="measurement-note">
          {measurementNote(result)} <span className="dim">Time is wall-clock around one search with tracing off; loading the graph is not included. Method order was shuffled for this run.</span>
        </p>
      </section>

      <section className="panel stack" aria-labelledby="ops-h">
        <div>
          <h2 id="ops-h">Algorithmic operation counts</h2>
          <p className="small muted" style={{ margin: 0 }}>
            How much work each method did. These are counts of operations, not time: fewer operations is not always faster, because
            different queues pay different amounts per operation.
          </p>
        </div>
        <div className="scroll-x">
          <table className="data counts" data-testid="counts-table">
            <thead>
              <tr>
                <th>Method</th>
                {countsRows(ok[0] ?? lanes[0]).map((r) => (
                  <th key={r.label} className="num">{r.label}<Tip text={r.tip} /></th>
                ))}
              </tr>
            </thead>
            <tbody>
              {ok.map((l) => (
                <tr key={l.id}>
                  <td>{laneLabel(l)}</td>
                  {countsRows(l).map((r) => (
                    <td key={r.label} className="num">{r.value}</td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>

      {result.effects.length ? (
        <section className="panel stack" aria-labelledby="fx-h">
          <div>
            <h2 id="fx-h">What made the difference?</h2>
            <p className="small muted" style={{ margin: 0 }}>
              Three separate things can change the speed. Each line below changes only one of them.
            </p>
          </div>
          <div className="row" style={{ gap: 6 }}>
            <span className="chip">Search algorithm<Tip text={ALGORITHM_TIP.dijkstra + " " + ALGORITHM_TIP.astar} /></span>
            <span className="chip">Priority queue<Tip text={QUEUE_TIP} /></span>
            <span className="chip ml-on">Machine learning<Tip text={ML_TIP} /></span>
          </div>
          <ul style={{ margin: 0, paddingLeft: 18 }} data-testid="effects">
            {result.effects.map((e) => (
              <li key={`${e.kind}-${e.lane_id}`} style={{ marginBottom: 4 }}>{effectSentence(e, result, labelOf)}</li>
            ))}
          </ul>
        </section>
      ) : null}

      <details className="advanced" data-testid="results-advanced">
        <summary>Advanced: spread, queue internals and run details</summary>
        <div className="adv-body stack">
          <p className="small muted" style={{ margin: 0 }}>
            Order seed {m.order_seed} (round r is shuffled with seed + r). {m.round_orders.map((o, i) => `Round ${i + 1}: ${o.map(labelOf).join(" → ")}`).join(". ")}. {m.execution}.
            {m.all_pinned ? ` Pinned to core ${m.cores.join(", ")}.` : " At least one lane was not pinned to a core, so its timing may carry scheduler noise."}
            {result.heuristic.used_by_astar_lanes ? ` Both A* lanes used the ${result.heuristic.used_by_astar_lanes} distance estimate.` : ""}
          </p>
          <div className="scroll-x">
            <table className="data counts">
              <thead>
                <tr>
                  <th>Method</th><th className="num">Median</th><th className="num">± MAD</th><th className="num">Relative spread</th>
                  <th className="num">Within-run MAD</th><th className="num">Between-round MAD</th><th className="num">Min</th><th className="num">Max</th><th className="num">Pushes</th><th className="num">Decrease-keys</th>
                  <th className="num">Stale pops</th><th className="num">Peak queue</th><th className="num">Edges examined</th>
                </tr>
              </thead>
              <tbody>
                {ok.map((l) => (
                  <tr key={l.id}>
                    <td>{laneLabel(l)}{l.timing?.noisy ? <span className="badge warn" style={{ marginLeft: 6 }}>noisy</span> : null}</td>
                    <td className="num">{fmtNs(l.timing?.median_ns)}</td>
                    <td className="num">{fmtNs(l.timing?.mad_ns)}</td>
                    <td className="num">{fmtPct(l.timing?.relative_mad ?? null)}</td>
                    <td className="num">{fmtNs(l.timing?.mad_within_ns)}</td>
                    <td className="num">{fmtNs(l.timing?.mad_between_rounds_ns)}</td>
                    <td className="num">{fmtNs(l.timing?.min_ns)}</td>
                    <td className="num">{fmtNs(l.timing?.max_ns)}</td>
                    <td className="num">{fmtInt(l.counts?.pushes)}</td>
                    <td className="num">{fmtInt(l.counts?.decrease_keys)}</td>
                    <td className="num">{fmtInt(l.counts?.stale_pops)}</td>
                    <td className="num">{fmtInt(l.counts?.peak_queue_size)}</td>
                    <td className="num">{fmtInt(l.counts?.edges_examined)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {ok.filter((l) => l.lpq).map((l) => (
            <div key={l.id}>
              <h3>{laneLabel(l)}: queue statistics reported by the engine</h3>
              <pre className="mono small" style={{ margin: 0, overflowX: "auto" }}>{JSON.stringify((l.lpq as { metrics?: unknown })?.metrics ?? l.lpq, null, 1)}</pre>
            </div>
          ))}
          <p className="small dim" style={{ margin: 0 }}>{result.noise_rule}</p>
        </div>
      </details>
    </div>
  );
}
