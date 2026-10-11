import { describe, expect, it } from "vitest";
import { LANES, defaultLaneIds, laneDef, lanesForRequest, unavailableLaneIds } from "./lanes";

describe("lane config", () => {
  it("has unique ids and unique algorithm+queue pairs, as the API requires", () => {
    expect(new Set(LANES.map((l) => l.id)).size).toBe(LANES.length);
    expect(new Set(LANES.map((l) => `${l.algorithm}/${l.queue}`)).size).toBe(LANES.length);
    for (const l of LANES) expect(l.id).toMatch(/^[a-z0-9_]{1,64}$/);
  });
  it("gives every algorithm that has an optimized lane a vanilla binary-heap lane", () => {
    for (const l of LANES.filter((x) => x.kind === "optimized")) {
      expect(LANES.some((v) => v.kind === "vanilla" && v.algorithm === l.algorithm && v.queue === "binary_heap")).toBe(true);
    }
  });
  it("defaults to the four core lanes plus the radix heap, the strongest non-ML baseline", () => {
    expect(defaultLaneIds().sort()).toEqual(["optimized_astar", "optimized_dijkstra", "radix_dijkstra", "vanilla_astar", "vanilla_dijkstra"]);
    expect(LANES.filter((l) => l.kind === "baseline").every((l) => l.optional)).toBe(true);
  });
  it("builds the request from ticked ids in config order, with labels", () => {
    const body = lanesForRequest(["radix_dijkstra", "vanilla_dijkstra"]);
    expect(body.map((l) => l.id)).toEqual(["vanilla_dijkstra", "radix_dijkstra"]);
    expect(body[1]).toEqual({ id: "radix_dijkstra", algorithm: "dijkstra", queue: "radix_heap", label: "Radix heap Dijkstra" });
    expect(laneDef("nope")).toBeUndefined();
  });
  it("flags only lanes whose queue the engine says it lacks", () => {
    const gone = unavailableLaneIds([
      { id: "lpq_optimized", engine_available: false },
      { id: "radix_heap", engine_available: true },
      { id: "four_ary_heap", engine_available: null },
    ]);
    expect([...gone].sort()).toEqual(["optimized_astar", "optimized_dijkstra"]);
    expect(unavailableLaneIds([]).size).toBe(0);
  });
});
