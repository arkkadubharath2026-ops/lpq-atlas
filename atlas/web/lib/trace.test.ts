import { describe, expect, it } from "vitest";
import { NODE_FRONTIER, NODE_SETTLED, Replay, explain, type TraceEvent } from "./trace";

let seq = 0;
const ev = (type: string, fields: Record<string, unknown> = {}): TraceEvent => ({
  schema_version: 1,
  seq: ++seq,
  type,
  ...fields,
});

const small = (): TraceEvent[] => {
  seq = 0;
  return [
    ev("RUN_STARTED", { algorithm: "dijkstra", queue: "lpq", source: 0, target: 2 }),
    ev("SOURCE_INITIALIZED", { node: 0, distance: 0 }),
    ev("LPQ_BUCKET_SELECTED", { bucket_base: 0, bucket_width: 4, bucket_size: 1, bound: 0, queue_size: 1 }),
    ev("LPQ_CANDIDATE_WINDOW", { front: [{ node: 0, key: 0, score: 0.5 }], front_shown: 1 }),
    ev("LPQ_VERIFICATION_ACCEPTED", { node: 0, key: 0, comparisons: 1, entries_skipped: 0, resolution: "first_attempt" }),
    ev("NODE_SETTLED", { node: 0, distance: 0 }),
    ev("EDGE_RELAXED", { from: 0, to: 1, old_distance: null, new_distance: 3, key: 3 }),
    ev("EDGE_RELAXED", { from: 0, to: 2, old_distance: null, new_distance: 9, key: 9 }),
    ev("LPQ_BUCKET_SELECTED", { bucket_base: 0, bucket_width: 4, bucket_size: 1, bound: 3, queue_size: 2 }),
    ev("LPQ_FALLBACK_STARTED", { reason: "verification_failed" }),
    ev("NODE_SETTLED", { node: 1, distance: 3 }),
    ev("EDGE_RELAXED", { from: 1, to: 2, old_distance: 9, new_distance: 5, key: 5 }),
    ev("NODE_SETTLED", { node: 2, distance: 5 }),
    ev("DESTINATION_REACHED", { node: 2, distance: 5 }),
    ev("PATH_RECONSTRUCTED", { path: [0, 1, 2], distance: 5 }),
    ev("RUN_COMPLETED", { termination: "target_reached", truncated: false }),
  ];
};

describe("Replay", () => {
  it("applies only the events it is given, in order", () => {
    const r = new Replay(small(), 3);
    r.seek(7); // through the first relaxation
    expect(r.nodeState[0]).toBe(NODE_SETTLED);
    expect(r.nodeState[1]).toBe(NODE_FRONTIER);
    expect(r.nodeState[2]).toBe(0);
    expect(r.lpq.accepted).toBe(1);
    while (r.step());
    expect(r.settledCount).toBe(3);
    expect(r.lpq.fallbacks).toBe(1);
    expect(r.lpq.extractions).toBe(2);
    expect(r.completed).toBe(true);
    expect([...r.nodeState]).toEqual([NODE_SETTLED, NODE_SETTLED, NODE_SETTLED]);
  });

  it("seeking backwards replays from the start to the same state", () => {
    const r = new Replay(small(), 3);
    r.seek(12);
    const snapshot = [...r.nodeState];
    r.seek(16);
    r.seek(12);
    expect([...r.nodeState]).toEqual(snapshot);
    expect(r.settledCount).toBe(2);
  });

  it("advances by settle count, the unit compare mode synchronises on", () => {
    const r = new Replay(small(), 3);
    r.advanceSettled(2);
    expect(r.settledCount).toBe(2);
    expect(r.lastSettled).toEqual({ node: 1, distance: 3 });
  });

  it("reports dirty nodes for incremental rendering", () => {
    const r = new Replay(small(), 3);
    r.seek(7);
    expect(new Set(r.drainDirty())).toEqual(new Set([0, 1]));
    expect(r.drainDirty()).toEqual([]);
  });

  it("explains events from their own fields", () => {
    const [, , , , , settle, relax] = small();
    expect(explain(settle, "dijkstra")).toContain("Node 0 is settled at distance 0");
    expect(explain(relax, "dijkstra")).toContain("from ∞ to 3");
  });
});
