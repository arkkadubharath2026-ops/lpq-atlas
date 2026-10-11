import { describe, expect, it } from "vitest";
import {
  barPercent,
  correctnessMessage,
  effectSentence,
  fmtFactor,
  headlineSentences,
  labelResolver,
  rankedLanes,
  verdictClause,
  vsVanillaPhrase,
} from "./compare";
import type { LaneComparisonResult, LaneResult } from "./types";

// Synthetic API responses: these test the display logic only. The numbers are
// invented for the test and say nothing about real performance.
function lane(p: Partial<LaneResult> & Pick<LaneResult, "id" | "algorithm" | "queue">): LaneResult {
  return {
    label: null,
    family: p.queue.startsWith("lpq") ? "learned" : "classical",
    run_index: 1,
    run_id: p.id,
    status: "ok",
    error: null,
    timing: {
      median_ns: 1000,
      mad_ns: 10,
      relative_mad: 0.01,
      min_ns: 990,
      max_ns: 1100,
      samples_ns: [],
      repetitions: 7,
      warmup: 2,
      noisy: false,
      pinned: true,
      core: 1,
      warnings: [],
    },
    matches_reference: true,
    effect: p.queue === "binary_heap" ? "vanilla" : p.queue.startsWith("lpq") ? "ml" : "queue",
    vanilla_lane_id: null,
    speedup_vs_vanilla: null,
    verdict: "unavailable",
    within_noise: null,
    rank: null,
    vs_fastest: null,
    ...p,
  };
}

function result(lanes: LaneResult[], extra: Partial<LaneComparisonResult> = {}): LaneComparisonResult {
  return {
    schema_version: 1,
    dataset_id: "d",
    source_node: 1,
    target_node: 2,
    measurement: {
      repetitions: 7, warmup: 2, tracing: "off", execution: "sequential", run_order: lanes.map((l) => l.id),
      order_seed: 1, all_pinned: true, cores: [1], clock: "", machine: { os: "Linux", arch: "x86_64" },
      elapsed_s: 1, contended_lanes: [],
    },
    heuristic: { requested: "auto", used_by_astar_lanes: "haversine", shared: true },
    lanes,
    path: [],
    fastest: { lane_id: lanes[0].id, median_ns: 1000, tied_with: [], clear_win: true },
    correctness: {
      checked_lanes: lanes.length, failed_lanes: [], references_agree: true, all_lanes_same_distance: true,
      all_match_reference: true, mismatching_lanes: [], reference_distance: 968015, reachable: true,
    },
    effects: [],
    noise_rule: "",
    vanilla_queue: "binary_heap",
    ...extra,
  };
}

const vanilla = lane({ id: "vd", label: "Vanilla Dijkstra", algorithm: "dijkstra", queue: "binary_heap", verdict: "reference", speedup_vs_vanilla: 1, vanilla_lane_id: "vd", rank: 2 });
const fasterOpt = lane({ id: "od", label: "Optimized Dijkstra", algorithm: "dijkstra", queue: "lpq_optimized", verdict: "faster", speedup_vs_vanilla: 1.12, vanilla_lane_id: "vd", rank: 1 });
const slowerOpt = lane({ id: "od", label: "Optimized Dijkstra", algorithm: "dijkstra", queue: "lpq_optimized", verdict: "slower", speedup_vs_vanilla: 0.8, vanilla_lane_id: "vd", rank: 2 });
const sameOpt = lane({ id: "od", label: "Optimized Dijkstra", algorithm: "dijkstra", queue: "lpq_optimized", verdict: "same", speedup_vs_vanilla: 1.01, vanilla_lane_id: "vd", rank: 1 });

describe("fmtFactor / verdictClause", () => {
  it("formats factors", () => {
    expect(fmtFactor(1.123)).toBe("1.12x");
    expect(fmtFactor(12.34)).toBe("12.3x");
    expect(fmtFactor(null)).toBe("—");
    expect(fmtFactor(0)).toBe("—");
  });
  it("says faster using the API ratio as given", () => {
    expect(verdictClause("faster", 1.12)).toBe("1.12x faster");
  });
  it("says slower using the reciprocal, never a ratio below 1", () => {
    expect(verdictClause("slower", 0.8)).toBe("1.25x slower");
  });
  it("never turns a within-noise result into a win or a loss", () => {
    expect(verdictClause("same", 1.03)).toBe("about the same speed");
    expect(verdictClause("same", 0.97)).toBe("about the same speed");
  });
  it("has nothing to say without a ratio", () => {
    expect(verdictClause("faster", null)).toBe("not comparable");
  });
});

describe("headlineSentences", () => {
  it("reports a win with the API's factor and names the fastest lane", () => {
    const r = result([fasterOpt, vanilla]);
    const s = headlineSentences(r, labelResolver(r));
    expect(s[0]).toBe("Optimized Dijkstra was 1.12x faster than Vanilla Dijkstra on this query.");
    expect(s[s.length - 1]).toBe("Optimized Dijkstra was fastest overall.");
  });
  it("says plainly when the optimized lane was slower", () => {
    const r = result([vanilla, slowerOpt], { fastest: { lane_id: "vd", median_ns: 1, tied_with: [], clear_win: true } });
    const s = headlineSentences(r, labelResolver(r));
    expect(s[0]).toBe("Optimized Dijkstra was 1.25x slower than Vanilla Dijkstra on this query. It was not faster here.");
    expect(s[1]).toBe("Vanilla Dijkstra was fastest overall.");
  });
  it("says it was not clearly faster when inside the noise", () => {
    const r = result([sameOpt, vanilla], { fastest: { lane_id: "od", median_ns: 1, tied_with: ["vd"], clear_win: false } });
    const s = headlineSentences(r, labelResolver(r));
    expect(s[0]).toContain("about the same speed");
    expect(s[0]).toContain("not clearly faster");
    expect(s[1]).toBe("Optimized Dijkstra and Vanilla Dijkstra were effectively tied for fastest (the gaps are within measurement noise).");
  });
  it("reports a failed optimized lane with its error and still names a winner", () => {
    const failed = lane({ id: "od", label: "Optimized Dijkstra", algorithm: "dijkstra", queue: "lpq_optimized", status: "error", error: { category: "engine", message: "unknown queue" }, timing: null, matches_reference: null });
    const r = result([vanilla, failed], { fastest: { lane_id: "vd", median_ns: 1, tied_with: [], clear_win: true } });
    const s = headlineSentences(r, labelResolver(r));
    expect(s[0]).toBe("Optimized Dijkstra could not run: unknown queue.");
    expect(s[1]).toBe("Vanilla Dijkstra was fastest overall.");
  });
  it("names a non-ML winner without crediting ML", () => {
    const radix = lane({ id: "rd", label: "Radix heap Dijkstra", algorithm: "dijkstra", queue: "radix_heap", verdict: "faster", speedup_vs_vanilla: 2, vanilla_lane_id: "vd", rank: 1 });
    const r = result([radix, vanilla, slowerOpt], { fastest: { lane_id: "rd", median_ns: 1, tied_with: [], clear_win: true } });
    const s = headlineSentences(r, labelResolver(r));
    expect(s.at(-1)).toBe("Radix heap Dijkstra was fastest overall.");
    expect(s[0]).toContain("slower");
  });
  it("handles no finished lane", () => {
    const r = result([vanilla], { fastest: null });
    expect(headlineSentences(r, labelResolver(r)).at(-1)).toContain("No lane finished");
  });
});

describe("vsVanillaPhrase", () => {
  const r = result([vanilla, fasterOpt]);
  const labelOf = labelResolver(r);
  it("phrases faster / slower / same / baseline", () => {
    expect(vsVanillaPhrase(fasterOpt, labelOf)).toEqual({ text: "1.12x faster than Vanilla Dijkstra", tone: "good" });
    expect(vsVanillaPhrase(slowerOpt, labelOf)).toEqual({ text: "1.25x slower than Vanilla Dijkstra", tone: "bad" });
    expect(vsVanillaPhrase(sameOpt, labelOf).tone).toBe("neutral");
    expect(vsVanillaPhrase(sameOpt, labelOf).text).toContain("within measurement noise");
    expect(vsVanillaPhrase(vanilla, labelOf).tone).toBe("neutral");
  });
  it("marks a failed lane", () => {
    const f = lane({ id: "x", algorithm: "dijkstra", queue: "lpq_optimized", status: "error", timing: null });
    expect(vsVanillaPhrase(f, labelOf)).toEqual({ text: "Did not run", tone: "bad" });
  });
});

describe("correctnessMessage", () => {
  it("passes when every lane agrees", () => {
    const m = correctnessMessage(result([vanilla, fasterOpt]));
    expect(m.tone).toBe("ok");
    expect(m.text).toBe("Both implementations returned the same optimal distance: 968,015.");
    expect(correctnessMessage(result([vanilla, fasterOpt, sameOpt, slowerOpt])).text).toContain("All 4 implementations");
  });
  it("warns, not passes, when a lane is missing", () => {
    const r = result([vanilla], { correctness: { ...result([vanilla]).correctness, failed_lanes: ["od"] } });
    const m = correctnessMessage(r);
    expect(m.tone).toBe("warn");
    expect(m.text).toContain("One lane did not run");
  });
  it("fails loudly on a mismatch", () => {
    const r = result([vanilla, fasterOpt], { correctness: { ...result([vanilla]).correctness, all_match_reference: false, mismatching_lanes: ["od"] } });
    const m = correctnessMessage(r);
    expect(m.tone).toBe("bad");
    expect(m.text).toContain("od");
  });
  it("reports an unreachable pair as agreement", () => {
    const r = result([vanilla, fasterOpt], { correctness: { ...result([vanilla]).correctness, reachable: false, reference_distance: null } });
    expect(correctnessMessage(r).text).toContain("no route");
  });
  it("cannot check with nothing finished", () => {
    const r = result([vanilla], { correctness: { ...result([vanilla]).correctness, checked_lanes: 0 } });
    expect(correctnessMessage(r).tone).toBe("warn");
  });
});

describe("rankedLanes / barPercent", () => {
  it("orders placed lanes by rank, then unplaced, then failed", () => {
    const failed = lane({ id: "f", algorithm: "dijkstra", queue: "radix_heap", status: "error", timing: null, run_index: 1 });
    const wrong = lane({ id: "w", algorithm: "dijkstra", queue: "four_ary_heap", rank: null, run_index: 2 });
    const r = result([failed, wrong, { ...vanilla, rank: 2 }, { ...fasterOpt, rank: 1 }]);
    expect(rankedLanes(r).map((l) => l.id)).toEqual(["od", "vd", "w", "f"]);
  });
  it("scales bars to the slowest lane with a visible minimum", () => {
    expect(barPercent(500, 1000)).toBe(50);
    expect(barPercent(1000, 1000)).toBe(100);
    expect(barPercent(1, 1000)).toBe(2);
    expect(barPercent(5, 0)).toBe(0);
  });
});

describe("effectSentence", () => {
  it("keeps algorithm, queue and ML effects separate", () => {
    const astar = lane({ id: "va", algorithm: "astar", queue: "binary_heap" });
    const radix = lane({ id: "rd", algorithm: "dijkstra", queue: "radix_heap" });
    const r = result([vanilla, astar, radix, fasterOpt]);
    const labelOf = labelResolver(r);
    expect(effectSentence({ kind: "algorithm", lane_id: "va", versus_lane_id: "vd", speedup: 2, verdict: "faster" }, r, labelOf))
      .toBe("Search algorithm: A* instead of Dijkstra (both with the binary heap) was 2.00x faster.");
    expect(effectSentence({ kind: "queue", lane_id: "rd", versus_lane_id: "vd", speedup: 1.5, verdict: "faster" }, r, labelOf))
      .toBe("Priority queue: Radix heap instead of the binary heap, with Dijkstra, was 1.50x faster.");
    expect(effectSentence({ kind: "ml", lane_id: "od", versus_lane_id: "vd", speedup: 0.8, verdict: "slower" }, r, labelOf))
      .toBe("ML: the Optimized queue instead of the binary heap, with Dijkstra, was 1.25x slower.");
    expect(effectSentence({ kind: "ml_vs_best_classical", lane_id: "od", versus_lane_id: "rd", speedup: 1.01, verdict: "same" }, r, labelOf))
      .toContain("about the same speed (1.01x, within noise)");
  });
});
