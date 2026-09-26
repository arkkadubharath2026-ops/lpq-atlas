"use client";
import { explain, type Replay, type TraceEvent } from "@/lib/trace";
import type { RunResult } from "@/lib/types";
import { fmtDistance, fmtInt, fmtNs, fmtNum, fmtPct, isLearned, queueLabel } from "@/lib/format";

/** Authoritative metrics: every number here is read from the engine's result file. */
export function Telemetry({ r, unit }: { r: RunResult; unit?: string }) {
  const b = r.timing.benchmark;
  const c = r.counters;
  const corr = r.correctness;
  return (
    <div className="stack" data-testid="telemetry">
      <div className="row" style={{ justifyContent: "space-between" }}>
        <h3 style={{ margin: 0 }}>Result</h3>
        <span className={`badge ${isLearned(r.request.queue) ? "learned" : "classical"}`}>
          {r.request.algorithm === "astar" ? `A* (${r.heuristic?.name})` : "Dijkstra"} · {queueLabel(r.request.queue)}
        </span>
      </div>
      {!r.result.reachable ? (
        <div className="notice warn">
          <strong>Destination unreachable</strong> from the source in this graph
          {r.reference.reachable ? "" : " (confirmed by the reference search)"}. No distance or ratio exists.
        </div>
      ) : null}
      <dl className="kv">
        <dt>Distance</dt>
        <dd data-testid="distance">{fmtDistance(r.result.distance, unit)}</dd>
        <dt>Reference (exact)</dt>
        <dd>{fmtDistance(r.reference.distance, unit)}</dd>
        <dt>Path</dt>
        <dd>{r.result.reachable ? `${fmtInt(r.result.path_hops)} hops` : "—"}</dd>
        <dt>Path cost verified</dt>
        <dd>{r.result.reachable ? (r.result.path_cost_verified ? "yes" : "NO") : "—"}</dd>
      </dl>
      <div className="row">
        {corr.policy === "exact" ? (
          <span className={`badge ${corr.exact_match ? "ok" : "bad"}`}>
            {corr.exact_match ? "exact: matches reference" : "exact: MISMATCH"}
          </span>
        ) : (
          <span className={`badge ${corr.bound_holds ? "ok" : "bad"}`}>
            ratio {corr.approximation_ratio === null ? "n/a" : fmtNum(corr.approximation_ratio, 6)} ≤ {fmtNum(corr.bound, 4)}{" "}
            {corr.bound_holds ? "holds" : "VIOLATED"}
          </span>
        )}
        <span className="badge">{r.result.termination.replace(/_/g, " ")}</span>
      </div>

      <h3>Benchmark timing</h3>
      {b ? (
        <>
          <dl className="kv">
            <dt>Median (untraced)</dt>
            <dd data-testid="median">{fmtNs(b.median_ns)}</dd>
            <dt>MAD</dt>
            <dd>{fmtNs(b.mad_ns)} ({fmtPct(b.relative_mad)})</dd>
            <dt>Min / max</dt>
            <dd>{fmtNs(b.min_ns)} / {fmtNs(b.max_ns)}</dd>
            <dt>Samples</dt>
            <dd>{b.repetitions} (+{b.warmup} warm-up)</dd>
            <dt>Pinned</dt>
            <dd>{b.pinned ? `core ${b.core}` : "no"}</dd>
          </dl>
          {b.noisy || b.warnings.length ? (
            <div className="notice warn small">{b.warnings.join(" ") || "Noisy measurement."}</div>
          ) : null}
        </>
      ) : (
        <p className="small muted">Timing disabled for this run (0 repetitions).</p>
      )}
      <p className="small dim" title={r.timing.clock_boundary}>
        Timed with tracing OFF. The animated replay was a separate, instrumented execution
        {r.timing.phases_ns.instrumented_trace_run ? ` (${fmtNs(r.timing.phases_ns.instrumented_trace_run)} incl. trace writing)` : ""} and is
        never used as the runtime.
      </p>

      <h3>Operations</h3>
      {c ? (
        <dl className="kv">
          <dt>Settled</dt><dd>{fmtInt(c.settled_nodes)}</dd>
          <dt>Edges examined</dt><dd>{fmtInt(c.edges_examined)}</dd>
          <dt>Edges relaxed</dt><dd>{fmtInt(c.edges_relaxed)}</dd>
          <dt>Pushes</dt><dd>{fmtInt(c.pq_pushes)}</dd>
          <dt>Pops</dt><dd>{fmtInt(c.pq_pops)}</dd>
          <dt>Decrease-keys</dt><dd>{fmtInt(c.pq_decrease_keys)}</dd>
          <dt>Stale pops</dt><dd>{fmtInt(c.stale_pops)}</dd>
          <dt>Comparisons</dt><dd>{fmtInt(c.pq_comparisons)}</dd>
          <dt>Peak queue</dt><dd>{fmtInt(c.peak_queue_size)}</dd>
        </dl>
      ) : (
        <p className="small muted">Counters were compiled out of this engine build.</p>
      )}

      {r.lpq ? <LpqResultPanel r={r} /> : null}

      <h3>Hardware counters</h3>
      <p className="small muted">{r.hardware_counters.reason}</p>
      <h3>Provenance</h3>
      <dl className="kv small">
        <dt>Run</dt><dd>{r.run_id}</dd>
        <dt>Dataset</dt><dd>{r.dataset.id}</dd>
        <dt>Checksum</dt><dd>{r.dataset.checksum_fnv1a64}</dd>
        <dt>Commit</dt><dd>{String(r.environment.git_commit).slice(0, 10)}{r.environment.git_dirty ? " (dirty)" : ""}</dd>
        <dt>Compiler</dt><dd>{String(r.environment.compiler)} {String(r.environment.compiler_version)}</dd>
        <dt>Build</dt><dd>{String(r.environment.build_type)} {String(r.environment.optimization_flags)}</dd>
      </dl>
    </div>
  );
}

function LpqResultPanel({ r }: { r: RunResult }) {
  const l = r.lpq!;
  const m = l.metrics;
  return (
    <>
      <h3>Learned queue</h3>
      <div className="row">
        <span className="badge learned">
          {l.scorer === "static_precomputed_model_scores" ? "static precomputed model scores" : "constant control scorer (no model)"}
        </span>
        {l.adaptive_controller ? <span className="badge learned">adaptive controller</span> : null}
        {l.online_learning ? <span className="badge learned">online learning</span> : <span className="badge">no online learning</span>}
      </div>
      <dl className="kv">
        <dt>Extractions</dt><dd>{fmtInt(m.extractions)}</dd>
        <dt>Verified (no scan)</dt><dd>{fmtInt(m.verified_extractions)} ({fmtPct(m.verified_rate)})</dd>
        <dt>Fallback scans</dt><dd>{fmtInt(m.fallback_scans)} ({fmtPct(m.fallback_rate, 2)})</dd>
        <dt>Entries examined</dt><dd>{fmtInt(m.entries_examined)}</dd>
        <dt>Entries skipped</dt><dd>{fmtInt(m.entries_skipped)}</dd>
        <dt>Verif. comparisons</dt><dd>{fmtInt(r.lpq!.verification.comparisons)}</dd>
        <dt>Window expansions</dt><dd>{fmtInt(r.lpq!.candidate.expansions)}</dd>
        <dt>First-attempt / corrected / expanded</dt>
        <dd>{fmtInt(l.correction.first_attempt)} / {fmtInt(l.correction.first_attempt_corrected)} / {fmtInt(l.correction.expanded)}</dd>
        <dt>Bucket width / window</dt><dd>{fmtInt(l.bucket_width)} / {l.window[0]}–{l.window[1]}</dd>
        {l.scorer_build_ns !== undefined ? (<><dt>Score precomputation</dt><dd>{fmtNs(l.scorer_build_ns)} (outside timing)</dd></>) : null}
      </dl>
    </>
  );
}

/** Trace-derived view of the learned queue's current decision. Labelled as such. */
export function LpqLive({ replay }: { replay: Replay }) {
  const l = replay.lpq;
  const maxScore = Math.max(1e-9, ...l.front.map((f) => Math.abs(f.score)));
  return (
    <div className="stack">
      <h3 style={{ margin: 0 }}>LPQ decisions <span className="dim small">(from trace)</span></h3>
      <dl className="kv small">
        <dt>Current bucket</dt>
        <dd>{l.bucketBase === null ? "—" : `[${fmtInt(l.bucketBase)}, ${fmtInt(l.bucketBase + (l.bucketWidth ?? 0))})`}</dd>
        <dt>Bucket size</dt><dd>{fmtInt(l.bucketSize)}</dd>
        <dt>Bound</dt><dd>{fmtInt(l.bound)}</dd>
        <dt>Last outcome</dt><dd>{l.lastOutcome ?? "—"}</dd>
        <dt>Accepted / rejected</dt><dd>{fmtInt(l.accepted)} / {fmtInt(l.rejected)}</dd>
        <dt>Corrections / fallbacks</dt><dd>{fmtInt(l.corrections)} / {fmtInt(l.fallbacks)}</dd>
        <dt>Fallback rate so far</dt><dd>{l.extractions ? fmtPct(l.fallbacks / l.extractions, 2) : "—"}</dd>
        {l.policy ? (<><dt>Controller</dt><dd>{l.policy.use_learned_path ? "learned path" : "RETREAT"} · window {l.policy.window_size}</dd></>) : null}
      </dl>
      <div className="small muted">Candidate front (score used at push time)</div>
      <div className="front-list" aria-label="candidate window">
        {l.front.length === 0 ? <span className="dim">—</span> : null}
        {l.front.map((f, i) => (
          <div key={`${f.node}-${i}`} style={{ display: "grid", gridTemplateColumns: "70px 80px 1fr 60px", gap: 6, alignItems: "center" }}>
            <span style={{ color: f.node === l.lastAcceptedNode ? "var(--ok)" : undefined }}>#{f.node}</span>
            <span className="dim">key {fmtInt(f.key)}</span>
            <div className="bar" style={{ width: `${(Math.abs(f.score) / maxScore) * 100}%`, opacity: 0.4 + 0.6 * (i === 0 ? 1 : 0.5) }} />
            <span className="dim">{fmtNum(f.score, 3)}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

export function PlayerControls(p: {
  playing: boolean;
  setPlaying: (b: boolean) => void;
  speed: number;
  setSpeed: (n: number) => void;
  index: number;
  total: number;
  onStep: () => void;
  onStepSettle: () => void;
  onRestart: () => void;
  onSeek: (i: number) => void;
  onFinish: () => void;
  disabled?: boolean;
}) {
  return (
    <div className="player" role="toolbar" aria-label="Trace playback">
      <button type="button" onClick={() => p.setPlaying(!p.playing)} disabled={p.disabled} aria-label={p.playing ? "Pause" : "Play"}>
        {p.playing ? "❚❚ Pause" : "▶ Play"}
      </button>
      <button type="button" onClick={p.onStep} disabled={p.disabled}>Step event</button>
      <button type="button" onClick={p.onStepSettle} disabled={p.disabled}>Step node</button>
      <button type="button" onClick={p.onRestart} disabled={p.disabled}>Restart</button>
      <button type="button" onClick={p.onFinish} disabled={p.disabled}>End</button>
      <input type="range" min={0} max={p.total} value={p.index} disabled={p.disabled}
        aria-label="Replay position" onChange={(e) => p.onSeek(Number(e.target.value))} />
      <span className="small mono">{fmtInt(p.index)} / {fmtInt(p.total)}</span>
      <label className="small muted row" style={{ gap: 4 }}>
        speed
        <select value={p.speed} onChange={(e) => p.setSpeed(Number(e.target.value))} style={{ width: 110 }} disabled={p.disabled}>
          {[20, 200, 2000, 20000, 200000].map((s) => (
            <option key={s} value={s}>{s.toLocaleString()} ev/s</option>
          ))}
        </select>
      </label>
    </div>
  );
}

const cls = (t: string) =>
  t.startsWith("LPQ_FALLBACK") ? "t-FALLBACK" : t.includes("REJECTED") ? "t-REJ" : t.includes("ACCEPTED") ? "t-ACC" : t.startsWith("LPQ") ? "t-LPQ" : "";

export function EventLog({ events, index }: { events: TraceEvent[]; index: number }) {
  const from = Math.max(0, index - 14);
  const shown = events.slice(from, index);
  return (
    <div className="eventlog" aria-live="off" aria-label="Recent trace events">
      {shown.length === 0 ? <div>—</div> : null}
      {shown.map((e, i) => (
        <div key={e.seq} className={`${cls(e.type)} ${i === shown.length - 1 ? "cur" : ""}`}>
          {e.seq} {e.type}
          {e.node !== undefined ? ` n=${e.node}` : ""}
          {e.to !== undefined ? ` ${e.from}→${e.to}` : ""}
          {e.distance !== undefined ? ` d=${e.distance}` : ""}
          {e.new_distance !== undefined ? ` d=${e.new_distance}` : ""}
        </div>
      ))}
    </div>
  );
}

export function StepExplanation({ event, algorithm }: { event: TraceEvent | null; algorithm: string }) {
  return (
    <div className="explain" aria-live="polite" data-testid="explanation">
      {event ? explain(event, algorithm) : <span className="dim">Step through the trace to see why each node is chosen.</span>}
    </div>
  );
}

export function TraceStatus({ r, events }: { r: RunResult; events: TraceEvent[] | null }) {
  if (!r.trace.available)
    return (
      <div className="notice warn small">
        No trace for this run{r.trace.removed_reason ? `: ${r.trace.removed_reason}` : r.trace.level === "none" ? " (trace level none — benchmark configuration)" : ""}. Metrics are unaffected.
      </div>
    );
  return (
    <div className="row small">
      <span className={`badge ${r.trace.matches_untraced_run ? "ok" : "bad"}`}>
        {r.trace.matches_untraced_run ? "trace reproduces the measured run" : `trace mismatch: ${r.trace.mismatch}`}
      </span>
      <span className="badge">{fmtInt(events?.length ?? r.trace.events_written)} events · {r.trace.level}</span>
      {r.trace.truncated ? (
        <span className="badge warn" title="Visualisation shows the first events only; the result and metrics are complete.">
          truncated: {fmtInt(r.trace.events_dropped)} events not recorded
        </span>
      ) : null}
    </div>
  );
}
