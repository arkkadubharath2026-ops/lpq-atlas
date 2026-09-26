// Replays an engine trace (schema_version 1, see cpp/include/lpq/trace/).
//
// The replay only APPLIES events the engine emitted, in order. It never infers
// an operation that is not in the stream, and the authoritative final metrics
// always come from the run result, not from anything counted here: the counts
// kept below exist to drive the animation and are labelled as trace-derived.

export interface TraceEvent {
  schema_version: number;
  seq: number;
  type: string;
  [field: string]: unknown;
}

export const NODE_NONE = 0;
export const NODE_FRONTIER = 1; // relaxed, not yet settled
export const NODE_SETTLED = 2;

export interface FrontEntry {
  node: number;
  key: number;
  score: number;
}

export interface LpqView {
  bucketBase: number | null;
  bucketWidth: number | null;
  bucketSize: number | null;
  bound: number | null;
  front: FrontEntry[];
  lastOutcome: string | null;
  lastAcceptedNode: number | null;
  accepted: number;
  rejected: number;
  corrections: number;
  fallbacks: number;
  extractions: number;
  policy: { use_learned_path: boolean; window_size: number } | null;
}

const emptyLpq = (): LpqView => ({
  bucketBase: null,
  bucketWidth: null,
  bucketSize: null,
  bound: null,
  front: [],
  lastOutcome: null,
  lastAcceptedNode: null,
  accepted: 0,
  rejected: 0,
  corrections: 0,
  fallbacks: 0,
  extractions: 0,
  policy: null,
});

const num = (e: TraceEvent, k: string): number => e[k] as number;

export class Replay {
  readonly events: TraceEvent[];
  readonly nodeCount: number;
  index = 0; // number of events applied
  nodeState: Uint8Array;
  settledCount = 0;
  relaxedCount = 0;
  lastSettled: { node: number; distance: number } | null = null;
  lastRelaxed: { from: number; to: number; old: number | null; next: number; h?: number } | null =
    null;
  lpq: LpqView = emptyLpq();
  /** Nodes whose state changed since the last drain: lets a renderer draw incrementally. */
  dirty: number[] = [];
  /** Bumped whenever state changes non-incrementally (reset, seek): renderers redraw fully. */
  redrawToken = 0;
  completed = false;
  truncated = false;

  constructor(events: TraceEvent[], nodeCount: number) {
    this.events = events;
    this.nodeCount = nodeCount;
    this.nodeState = new Uint8Array(nodeCount);
  }

  reset(): void {
    this.index = 0;
    this.nodeState.fill(0);
    this.settledCount = 0;
    this.relaxedCount = 0;
    this.lastSettled = null;
    this.lastRelaxed = null;
    this.lpq = emptyLpq();
    this.dirty = [];
    this.redrawToken++;
    this.completed = false;
    this.truncated = false;
  }

  get done(): boolean {
    return this.index >= this.events.length;
  }

  private mark(node: number, state: number): void {
    if (node < 0 || node >= this.nodeCount) return;
    if (this.nodeState[node] === state) return;
    this.nodeState[node] = state;
    this.dirty.push(node);
  }

  /** Applies the next event; returns it, or null at the end. */
  step(): TraceEvent | null {
    if (this.done) return null;
    const e = this.events[this.index++];
    switch (e.type) {
      case "SOURCE_INITIALIZED":
        this.mark(num(e, "node"), NODE_FRONTIER);
        break;
      case "NODE_SETTLED": {
        const node = num(e, "node");
        this.mark(node, NODE_SETTLED);
        this.settledCount++;
        this.lastSettled = { node, distance: num(e, "distance") };
        break;
      }
      case "EDGE_RELAXED": {
        const to = num(e, "to");
        if (this.nodeState[to] !== NODE_SETTLED) this.mark(to, NODE_FRONTIER);
        this.relaxedCount++;
        this.lastRelaxed = {
          from: num(e, "from"),
          to,
          old: (e.old_distance as number | null) ?? null,
          next: num(e, "new_distance"),
          h: e.h as number | undefined,
        };
        break;
      }
      case "LPQ_BUCKET_SELECTED":
        this.lpq.extractions++;
        this.lpq.bucketBase = num(e, "bucket_base");
        this.lpq.bucketWidth = num(e, "bucket_width");
        this.lpq.bucketSize = num(e, "bucket_size");
        this.lpq.bound = num(e, "bound");
        this.lpq.front = [];
        break;
      case "LPQ_CANDIDATE_WINDOW":
        this.lpq.front = (e.front as FrontEntry[]) ?? [];
        break;
      case "LPQ_VERIFICATION_ACCEPTED":
        this.lpq.accepted++;
        this.lpq.lastOutcome = `accepted (${e.resolution as string})`;
        this.lpq.lastAcceptedNode = num(e, "node");
        break;
      case "LPQ_VERIFICATION_REJECTED":
        this.lpq.rejected++;
        this.lpq.lastOutcome = `rejected: ${e.reason as string}`;
        break;
      case "LPQ_CORRECTION_APPLIED":
        this.lpq.corrections++;
        break;
      case "LPQ_FALLBACK_STARTED":
        this.lpq.fallbacks++;
        this.lpq.lastOutcome = `fallback: ${e.reason as string}`;
        break;
      case "LPQ_ADAPTIVE_POLICY":
        this.lpq.policy = {
          use_learned_path: e.use_learned_path as boolean,
          window_size: num(e, "window_size"),
        };
        break;
      case "RUN_COMPLETED":
        this.completed = true;
        this.truncated = Boolean(e.truncated);
        break;
      default:
        break;
    }
    return e;
  }

  /** Applies events until `count` more NODE_SETTLED events have been applied (or the end). */
  advanceSettled(count: number): TraceEvent | null {
    const goal = this.settledCount + count;
    let last: TraceEvent | null = null;
    while (!this.done && this.settledCount < goal) last = this.step();
    return last;
  }

  /** Applies events up to (and including) absolute index `i`. */
  seek(i: number): void {
    if (i < this.index) this.reset();
    while (this.index < Math.min(i, this.events.length)) this.step();
    this.redrawToken++;
  }

  drainDirty(): number[] {
    const d = this.dirty;
    this.dirty = [];
    return d;
  }
}

const fmt = (v: unknown): string =>
  typeof v === "number" ? v.toLocaleString("en-US", { maximumFractionDigits: 3 }) : String(v);

/** A plain-language account of one event, built only from that event's fields. */
export function explain(e: TraceEvent, algorithm: string): string {
  switch (e.type) {
    case "RUN_STARTED":
      return `Run started: ${fmt(e.algorithm)} with ${fmt(e.queue)} from node ${fmt(e.source)} to node ${fmt(e.target)}.`;
    case "SOURCE_INITIALIZED":
      return `The source node ${fmt(e.node)} enters the queue with distance 0${
        e.h !== undefined ? ` and heuristic h = ${fmt(e.h)} (f = ${fmt(e.f)})` : ""
      }.`;
    case "NODE_SETTLED":
      return algorithm.startsWith("astar")
        ? `Node ${fmt(e.node)} is settled with g = ${fmt(e.distance)}: its f = g + h was the smallest key the queue returned, so no cheaper route to it can appear later (consistent heuristic).`
        : `Node ${fmt(e.node)} is settled at distance ${fmt(e.distance)}: it was the smallest tentative distance the queue returned, and with non-negative weights nothing found later can beat it.`;
    case "EDGE_RELAXED":
      return `Edge ${fmt(e.from)} → ${fmt(e.to)} improved the best known distance of ${fmt(e.to)} from ${
        e.old_distance === null ? "∞" : fmt(e.old_distance)
      } to ${fmt(e.new_distance)}${
        e.h !== undefined
          ? `; it is queued with f = g + h = ${fmt(e.new_distance)} + ${fmt(e.h)} = ${fmt(e.key)}`
          : ""
      }.`;
    case "EDGE_EXAMINED":
      return `Edge ${fmt(e.from)} → ${fmt(e.to)} (weight ${fmt(e.weight)}) examined; candidate distance ${fmt(e.candidate)}.`;
    case "QUEUE_POP":
      return `The queue returned node ${fmt(e.node)} with key ${fmt(e.key)} (${fmt(e.queue_size)} entries left).`;
    case "STALE_POP":
      return `Node ${fmt(e.node)} came out of the queue with an outdated key ${fmt(e.key)} and was discarded (lazy deletion).`;
    case "QUEUE_PUSH":
      return `Node ${fmt(e.node)} pushed with key ${fmt(e.key)}.`;
    case "LPQ_BUCKET_SELECTED":
      return `LPQ: the current bucket covers keys [${fmt(e.bucket_base)}, ${fmt(
        (e.bucket_base as number) + (e.bucket_width as number),
      )}) and holds ${fmt(e.bucket_size)} entries; nothing in the queue is below the bound ${fmt(e.bound)}.`;
    case "LPQ_CANDIDATE_WINDOW": {
      const f = (e.front as FrontEntry[]) ?? [];
      return `LPQ: candidate window built from the bucket's best-scored front (${f.length} shown). Scores were stored when each entry was pushed.`;
    }
    case "LPQ_CANDIDATE_SELECTED":
      return e.prediction_correct
        ? `LPQ: the top-scored candidate${e.node !== undefined ? ` (node ${fmt(e.node)})` : ""} was the true minimum.`
        : `LPQ: the top-scored candidate had key ${fmt(e.predicted_key)}, which was NOT the minimum (rank displacement ${fmt(e.rank_displacement)}).`;
    case "LPQ_VERIFICATION_ACCEPTED":
      return `LPQ: verification proved node ${fmt(e.node)} (key ${fmt(e.key)}) extractable using ${fmt(e.comparisons)} comparisons; ${fmt(e.entries_skipped)} bucket entries were never touched.`;
    case "LPQ_VERIFICATION_REJECTED":
      return `LPQ: verification rejected the window (${fmt(e.reason)}).`;
    case "LPQ_CORRECTION_APPLIED":
      return e.kind === "window_expanded"
        ? `LPQ: correction — the window was widened ${fmt(e.expansions)} time(s) before a candidate could be proven.`
        : `LPQ: correction — the minimum sat at rank ${fmt(e.rank_displacement)} in the window, not first.`;
    case "LPQ_FALLBACK_STARTED":
      return `LPQ: fallback to the classical bucket scan (${fmt(e.reason)}). The result stays exact/bounded; the cost is the full scan.`;
    case "LPQ_ADAPTIVE_POLICY":
      return `Adaptive controller: learned path ${e.use_learned_path ? "ON" : "OFF"}, window ${fmt(e.window_size)}.`;
    case "DESTINATION_REACHED":
      return `The destination ${fmt(e.node)} was settled at distance ${fmt(e.distance)}: the search stops.`;
    case "PATH_RECONSTRUCTED":
      return `Path reconstructed from predecessors: ${((e.path as number[]) ?? []).length} nodes, distance ${fmt(e.distance)}.`;
    case "RUN_COMPLETED":
      return `Run completed (${fmt(e.termination)}).${
        e.truncated ? ` The trace was capped: ${fmt(e.events_dropped)} events were not recorded.` : ""
      }`;
    default:
      return e.type;
  }
}
