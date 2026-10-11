// The comparison lanes: the ONE place that decides which (algorithm, priority
// queue) pairs the Quick Compare page races, what they are called and what the
// tooltips say. Add, remove or rename a lane here and nothing else changes: the
// request body, the labels, the explanations and the optional-lane checkboxes
// are all derived from this list.
//
// Rules the API enforces and this list must respect:
//   * a lane's id is [a-z0-9_]{1,64} and unique;
//   * each (algorithm, queue) pair appears once;
//   * a lane is "vanilla" when it pairs its algorithm with the binary heap --
//     speedups are measured against the vanilla lane of the SAME algorithm,
//     so keep one binary-heap lane per algorithm that is present.

export type LaneKind = "vanilla" | "optimized" | "baseline";

export interface LaneDef {
  /** Stable id, sent to the API and echoed back in the results. */
  id: string;
  /** Short plain-language name used in bars, tables and sentences. */
  label: string;
  algorithm: "dijkstra" | "astar";
  /** Engine queue id. */
  queue: string;
  kind: LaneKind;
  /** Optional lanes start unticked and can be switched on by the user. */
  optional: boolean;
  /** One plain sentence on what this lane is, for the tooltip. */
  tooltip: string;
  /** Used by the bars: a CSS colour variable. */
  color: string;
}

export const ALGORITHM_TIP: Record<LaneDef["algorithm"], string> = {
  dijkstra:
    "Dijkstra explores outward from the start in every direction, nearest places first, until it reaches the destination.",
  astar:
    "A* works like Dijkstra but also uses the straight-line distance to the destination to prefer places that point the right way. It usually visits far fewer places.",
};

export const QUEUE_TIP =
  "The priority queue is the to-do list the search keeps sorted: it hands back the nearest unvisited place next. How fast that list works changes the running time but never the answer.";

export const ML_TIP =
  "Machine learning here only gives the queue a hint about which places to look at next. Every hint is checked against an exact fallback, so it can change the speed but never the distance returned.";

export const LANES: readonly LaneDef[] = [
  {
    id: "vanilla_dijkstra",
    label: "Vanilla Dijkstra",
    algorithm: "dijkstra",
    queue: "binary_heap",
    kind: "vanilla",
    optional: false,
    tooltip:
      "Plain Dijkstra with an ordinary binary heap. This is the reference every Dijkstra lane is compared with.",
    color: "var(--classical)",
  },
  {
    id: "optimized_dijkstra",
    label: "Optimized Dijkstra",
    algorithm: "dijkstra",
    queue: "lpq_optimized",
    kind: "optimized",
    optional: false,
    tooltip:
      "The same Dijkstra search, but its priority queue is the ML-assisted Optimized queue. Same answer by construction; the question is whether it is quicker.",
    color: "var(--learned)",
  },
  {
    id: "vanilla_astar",
    label: "Vanilla A*",
    algorithm: "astar",
    queue: "binary_heap",
    kind: "vanilla",
    optional: false,
    tooltip:
      "Plain A* with an ordinary binary heap. This is the reference every A* lane is compared with.",
    color: "var(--classical)",
  },
  {
    id: "optimized_astar",
    label: "Optimized A*",
    algorithm: "astar",
    queue: "lpq_optimized",
    kind: "optimized",
    optional: false,
    tooltip:
      "The same A* search, but its priority queue is the ML-assisted Optimized queue. It uses the same distance estimate as Vanilla A*.",
    color: "var(--learned)",
  },
  {
    id: "radix_dijkstra",
    label: "Radix heap Dijkstra",
    algorithm: "dijkstra",
    queue: "radix_heap",
    kind: "baseline",
    optional: true,
    tooltip:
      "Dijkstra with a radix heap, a strong non-ML priority queue built for exactly this kind of search. A fair test of whether ML is needed to go faster.",
    color: "var(--primary)",
  },
  {
    id: "four_ary_dijkstra",
    label: "4-ary heap Dijkstra",
    algorithm: "dijkstra",
    queue: "four_ary_heap",
    kind: "baseline",
    optional: true,
    tooltip:
      "Dijkstra with a 4-ary heap, a cache-friendlier cousin of the binary heap and another strong non-ML baseline.",
    color: "var(--primary)",
  },
];

export const DEFAULT_REPETITIONS = 7;
export const DEFAULT_WARMUP = 2;
export const MIN_REPETITIONS = 3;
export const MIN_WARMUP = 2;

export const laneDef = (id: string): LaneDef | undefined => LANES.find((l) => l.id === id);

/** The lanes switched on by default. */
export const defaultLaneIds = (): string[] => LANES.filter((l) => !l.optional).map((l) => l.id);

export interface LaneRequestItem {
  id: string;
  algorithm: LaneDef["algorithm"];
  queue: string;
  label: string;
}

/** The lanes to send, in config order, for the ticked ids. */
export function lanesForRequest(ids: Iterable<string>): LaneRequestItem[] {
  const on = new Set(ids);
  return LANES.filter((l) => on.has(l.id)).map((l) => ({
    id: l.id,
    algorithm: l.algorithm,
    queue: l.queue,
    label: l.label,
  }));
}

/**
 * Lanes whose queue the engine build cannot run, from /api/queues. A lane is
 * only reported unavailable when the API says so (`engine_available === false`);
 * unknown stays runnable so that a real engine error is shown on the lane.
 */
export function unavailableLaneIds(
  queues: { id: string; engine_available?: boolean | null }[],
): Set<string> {
  const missing = new Set(queues.filter((q) => q.engine_available === false).map((q) => q.id));
  return new Set(LANES.filter((l) => missing.has(l.queue)).map((l) => l.id));
}
