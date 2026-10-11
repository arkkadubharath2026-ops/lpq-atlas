// Display logic for the Quick Compare page. Pure functions, no fetching and no
// arithmetic on measurements beyond presentation (a reciprocal to say "1.3x
// slower", a bar width). The speedup, the ranking, the fastest lane and the
// same-distance verdict are computed once, by the API, and only shown here.
import { fmtDistance, fmtInt, fmtNs, queueLabel } from "./format";
import type { LaneComparisonResult, LaneEffect, LaneResult, LaneVerdict, VerdictReason } from "./types";

/** "1.12x"; two decimals below 10, one above. */
export function fmtFactor(f: number | null | undefined): string {
  if (f === null || f === undefined || !Number.isFinite(f) || f <= 0) return "—";
  return `${f >= 10 ? f.toFixed(1) : f.toFixed(2)}x`;
}

/**
 * The comparison as a clause: "1.12x faster", "1.30x slower", "about the same
 * speed". `speedup` is the API's vanilla / lane ratio (above 1 = lane faster).
 * A verdict of "same" is the API's statement that the gap is within noise, so
 * it is never rewritten as a win or a loss here.
 */
export function verdictClause(verdict: LaneVerdict, speedup: number | null): string {
  if (speedup === null || speedup === undefined) return "not comparable";
  switch (verdict) {
    case "faster":
      return `${fmtFactor(speedup)} faster`;
    case "slower":
      return `${fmtFactor(1 / speedup)} slower`;
    case "same":
      return "about the same speed";
    default:
      return "not comparable";
  }
}

export type Tone = "good" | "bad" | "neutral";

export function verdictTone(verdict: LaneVerdict): Tone {
  return verdict === "faster" ? "good" : verdict === "slower" ? "bad" : "neutral";
}

export type LabelOf = (laneId: string) => string;

/** Lane names: the page's own lane config first, then the label the API echoed, then the id. */
export function labelResolver(
  result: LaneComparisonResult,
  preferred?: (laneId: string) => string | undefined,
): LabelOf {
  const byId = new Map(result.lanes.map((l) => [l.id, l.label ?? l.id]));
  return (id) => preferred?.(id) ?? byId.get(id) ?? id;
}

/** "ahead in 3 of 3 rounds" for the side of the comparison that won. */
export function roundsTag(
  x: { rounds_faster?: number | null; rounds_slower?: number | null; rounds_compared?: number | null },
  verdict: LaneVerdict,
): string {
  const n = x.rounds_compared ?? 0;
  if (!n) return "";
  const k = verdict === "slower" ? x.rounds_slower : x.rounds_faster;
  return `${k ?? 0} of ${n} rounds`;
}

/** Why the API called a gap "about the same", in plain words. */
function samePhrase(
  x: { speedup: number | null; reason?: VerdictReason; rounds_faster?: number | null; rounds_compared?: number | null },
  versus: string,
): string {
  const ratio = fmtFactor(x.speedup);
  if (x.reason === "single_round")
    return `Not enough evidence to call a winner against ${versus}: only one round was measured (${ratio} in that round)`;
  if (x.reason === "not_consistent_across_rounds")
    return `No clear difference from ${versus}: ahead in only ${x.rounds_faster ?? 0} of ${x.rounds_compared ?? 0} rounds (${ratio} overall)`;
  return `About the same speed as ${versus} (${ratio}, within measurement noise)`;
}

/** The text beside a bar: how this lane did against its vanilla counterpart. */
export function vsVanillaPhrase(
  lane: LaneResult,
  labelOf: LabelOf,
): { text: string; tone: Tone } {
  if (lane.status !== "ok") return { text: "Did not run", tone: "bad" };
  if (lane.verdict === "reference")
    return { text: "Baseline for the other lanes using this algorithm", tone: "neutral" };
  if (lane.verdict === "unavailable" || !lane.vanilla_lane_id)
    return { text: "No vanilla lane to compare with", tone: "neutral" };
  const v = labelOf(lane.vanilla_lane_id);
  if (lane.verdict === "same")
    return {
      text: samePhrase({ ...lane, speedup: lane.speedup_vs_vanilla, reason: lane.verdict_reason }, v),
      tone: "neutral",
    };
  const tag = roundsTag(lane, lane.verdict);
  return {
    text: `${verdictClause(lane.verdict, lane.speedup_vs_vanilla)} than ${v}${tag ? ` (ahead in ${tag})` : ""}`,
    tone: verdictTone(lane.verdict),
  };
}

/** What the Optimized queue resolved to and how, e.g. for a line under its bar. */
export function resolutionText(lane: LaneResult): string | null {
  if (!lane.resolved_queue) return null;
  const how =
    lane.resolution_method === "policy"
      ? "using a stored queue policy"
      : lane.resolution_method === "default"
        ? "by default"
        : "by measuring candidate queues on this graph and this machine";
  if (lane.resolved_to_radix)
    return `Optimized chose the radix heap here (no gain measured), so its time equals radix. ${how.replace(/^./, (c) => c.toUpperCase())}.`;
  return `Optimized chose ${lane.resolved_queue} ${how}.`;
}

/** The one-time set-up cost line, or null when nothing was calibrated for this lane. */
export function setupText(lane: LaneResult): string | null {
  const ns = lane.calibration_ns;
  if (ns === null || ns === undefined || !lane.resolved_queue) return null;
  if (ns > 0)
    return `One-time setup, not included in the time: calibrating the Optimized queue on this graph took ${fmtNs(ns)} (cached afterwards).`;
  return lane.resolution_method === "autotune_cached"
    ? "One-time setup: this graph was already calibrated on this machine (cached), so no calibration was needed."
    : null;
}

/** "31.0 ms · 31.3 ms · 32.1 ms": the per-round medians. */
export function fmtRoundMedians(l: LaneResult): string {
  const rs = l.timing?.round_medians_ns ?? [];
  return rs.map((x) => (x === null ? "failed" : fmtNs(x))).join(" · ");
}

/** The plain-language summary: what the optimized lanes did, then who was fastest. */
export function headlineSentences(result: LaneComparisonResult, labelOf: LabelOf): string[] {
  const out: string[] = [];
  for (const lane of result.lanes) {
    if (lane.family !== "learned") continue;
    const name = labelOf(lane.id);
    if (lane.status !== "ok") {
      out.push(`${name} could not run: ${lane.error?.message ?? "the engine reported an error"}.`);
      continue;
    }
    if (!lane.vanilla_lane_id || lane.speedup_vs_vanilla === null) {
      out.push(`${name} has no vanilla lane of the same algorithm to be compared with.`);
    } else {
      const v = labelOf(lane.vanilla_lane_id);
      const tag = roundsTag(lane, lane.verdict);
      if (lane.verdict === "faster")
        out.push(
          `${name} was ${fmtFactor(lane.speedup_vs_vanilla)} faster than ${v} on this query${tag ? ` (ahead in ${tag})` : ""}.`,
        );
      else if (lane.verdict === "slower")
        out.push(
          `${name} was ${fmtFactor(1 / lane.speedup_vs_vanilla)} slower than ${v} on this query${tag ? ` (behind in ${tag})` : ""}. It was not faster here.`,
        );
      else
        out.push(
          `${name} was not clearly faster than ${v} on this query: ${samePhrase({ speedup: lane.speedup_vs_vanilla, reason: lane.verdict_reason, rounds_faster: lane.rounds_faster, rounds_compared: lane.rounds_compared }, v).replace(/^./, (c) => c.toLowerCase())}.`,
        );
    }
    if (lane.resolved_to_radix) {
      out.push(
        `${name} chose the radix heap here (no gain measured), so its time equals radix. Any gain over the vanilla lane is credited to the queue choice, not to ML.`,
      );
    } else if (lane.resolved_queue) {
      out.push(`${name} chose ${lane.resolved_queue} by measuring candidate queues on this graph and machine.`);
    }
    const vsBest = result.effects.find((e) => e.kind === "ml_vs_best_classical" && e.lane_id === lane.id);
    if (vsBest && vsBest.speedup !== null) {
      const base = labelOf(vsBest.versus_lane_id);
      const tag = roundsTag(vsBest, vsBest.verdict);
      if (vsBest.verdict === "faster")
        out.push(`${name} beat ${base}, the strongest non-ML queue, by ${fmtFactor(vsBest.speedup)} (ahead in ${tag}).`);
      else if (vsBest.verdict === "slower")
        out.push(`${name} did not beat ${base}, the strongest non-ML queue: it was ${fmtFactor(1 / vsBest.speedup)} slower.`);
      else
        out.push(
          lane.resolved_to_radix
            ? `${name} did not beat ${base}: it used the same queue here.`
            : `${name} did not clearly beat ${base}, the strongest non-ML queue (about the same speed).`,
        );
    }
  }
  const f = result.fastest;
  if (f) {
    if (f.clear_win) out.push(`${labelOf(f.lane_id)} was fastest overall.`);
    else {
      const names = [f.lane_id, ...f.tied_with].map(labelOf);
      out.push(
        `${joinNames(names)} were effectively tied for fastest (the gaps are within measurement noise or not consistent across rounds).`,
      );
    }
  } else out.push("No lane finished, so there is no fastest method for this query.");
  return out;
}

function joinNames(names: string[]): string {
  if (names.length <= 1) return names[0] ?? "";
  if (names.length === 2) return `${names[0]} and ${names[1]}`;
  return `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}`;
}

/** The same-distance check as a sentence plus a tone for its banner. */
export function correctnessMessage(
  result: LaneComparisonResult,
  unit?: string,
): { text: string; tone: "ok" | "bad" | "warn" } {
  const c = result.correctness;
  const failed = c.failed_lanes.length
    ? ` ${c.failed_lanes.length === 1 ? "One lane" : `${c.failed_lanes.length} lanes`} did not run and ${c.failed_lanes.length === 1 ? "is" : "are"} not included.`
    : "";
  if (c.checked_lanes === 0)
    return { text: "No implementation finished, so correctness could not be checked.", tone: "warn" };
  if (c.all_match_reference === false || c.references_agree === false)
    return {
      text: `Warning: the implementations did not all return the same distance${c.mismatching_lanes.length ? ` (different: ${c.mismatching_lanes.join(", ")})` : ""}. Do not trust the timings for this query.`,
      tone: "bad",
    };
  const n = c.checked_lanes;
  const all = n === 2 ? "Both" : `All ${n}`;
  if (c.reachable === false)
    return {
      text: `${all} implementations agree that there is no route between these two places.${failed}`,
      tone: failed ? "warn" : "ok",
    };
  return {
    text: `${all} implementations returned the same optimal distance: ${fmtDistance(c.reference_distance, unit)}.${failed}`,
    tone: failed ? "warn" : "ok",
  };
}

/** Lanes in display order: placed lanes by rank, then unplaced, then failed. */
export function rankedLanes(result: LaneComparisonResult): LaneResult[] {
  const group = (l: LaneResult) => (l.rank !== null ? 0 : l.status === "ok" ? 1 : 2);
  return [...result.lanes].sort(
    (a, b) => group(a) - group(b) || (a.rank ?? 0) - (b.rank ?? 0) || 0,
  );
}

/** Bar length as a percent of the slowest placed lane, never invisibly short. */
export function barPercent(medianNs: number, slowestNs: number): number {
  if (!(slowestNs > 0) || !(medianNs > 0)) return 0;
  return Math.max(2, Math.min(100, (medianNs / slowestNs) * 100));
}

export function slowestMedian(result: LaneComparisonResult): number {
  return Math.max(0, ...result.lanes.map((l) => (l.timing ? l.timing.median_ns : 0)));
}

export const fmtMedian = (l: LaneResult): string => (l.timing ? fmtNs(l.timing.median_ns) : "—");

/** "± 120.1 µs" -- the median absolute deviation of the repeated runs. */
export const fmtSpread = (l: LaneResult): string =>
  l.timing ? `± ${fmtNs(l.timing.mad_ns)}` : "";

/** One effect as a sentence, e.g. "Switching from Dijkstra to A* ... was 3.88x faster". */
export function effectSentence(e: LaneEffect, result: LaneComparisonResult, labelOf: LabelOf): string {
  const lane = result.lanes.find((l) => l.id === e.lane_id);
  const tag = roundsTag(e, e.verdict);
  const clause = verdictClause(e.verdict, e.speedup);
  const tail =
    e.verdict === "same"
      ? ` (${fmtFactor(e.speedup)}${e.reason === "within_noise" ? ", within noise" : e.reason === "single_round" ? ", one round only" : ", not consistent across rounds"})`
      : tag
        ? ` (${tag})`
        : "";
  const alg = lane?.algorithm === "astar" ? "A*" : "Dijkstra";
  switch (e.kind) {
    case "algorithm":
      return `Search algorithm: A* instead of Dijkstra (both with the binary heap) was ${clause}${tail}.`;
    case "queue":
      if (lane?.resolved_to_radix)
        return `Queue choice: Optimized picked the radix heap here, so with ${alg} it behaves as the radix heap did against the binary heap: ${clause}${tail}. This is the queue's effect, not ML's.`;
      return `Priority queue: ${queueLabel(lane?.queue ?? "")} instead of the binary heap, with ${alg}, was ${clause}${tail}.`;
    case "ml":
      return `ML: the Optimized queue instead of the binary heap, with ${alg}, was ${clause}${tail}.`;
    case "ml_vs_best_classical":
      return `Optimized ${alg} against ${labelOf(e.versus_lane_id)} (the strongest non-ML queue) was ${clause}${tail}.`;
  }
}

/** The honest-labelling note shown with every result. */
export function measurementNote(result: LaneComparisonResult): string {
  const m = result.measurement;
  const rounds = m.rounds ?? 1;
  return (
    `These times were measured just now on this machine (${m.machine.os} ${m.machine.arch}) by the C++ engine, ` +
    `not copied from anywhere. A single query is noisy, and separate runs of the same method can drift on a shared machine, ` +
    `so every method was measured in ${rounds} round${rounds === 1 ? "" : "s"} (a fresh process each, order reshuffled every round), ` +
    `each with ${m.repetitions} timed repetitions after ${m.warmup} warm-up runs. ` +
    "The time shown is the median of the per-round medians, with ± the larger of the within-run and between-round spread. " +
    "Results on other machines or other routes can differ."
  );
}

export function countsRows(l: LaneResult): { label: string; value: string; tip: string }[] {
  const c = l.counts;
  const n = (v: number | null | undefined) => (v === null || v === undefined ? "—" : fmtInt(v));
  return [
    { label: "Nodes explored", value: n(c?.settled_nodes), tip: "Places the search finished with (settled) before it reached the destination." },
    { label: "Queue pops", value: n(c?.pops), tip: "Times the search asked the queue for the next place." },
    { label: "Queue comparisons", value: n(c?.comparisons), tip: "Key comparisons the queue made internally; this depends on how the queue is built." },
  ];
}
