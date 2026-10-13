import { describe, expect, it } from "vitest";
import { DEFAULT_CONFIG, modelUnavailableNotice, normaliseConfig } from "../components/RunConfigForm";

const online = { ...DEFAULT_CONFIG, queue: "lpq_online", model_id: "m1" };

describe("lpq_online request normalisation", () => {
  it("keeps an explicit model and the chosen update mode for a warm start", () => {
    const c = normaliseConfig({ ...online, online_update_mode: "disabled" });
    expect(c.model_id).toBe("m1");
    expect(c.online_update_mode).toBe("disabled");
    expect(c.online_start).toBe("warm");
  });
  it("sends no model and no override for a cold start", () => {
    const c = normaliseConfig({ ...online, online_start: "cold", allow_graph_mismatch: true, accept_no_provenance: true });
    expect(c.model_id).toBeNull();
    expect([c.allow_graph_mismatch, c.allow_width_mismatch, c.accept_no_provenance]).toEqual([false, false, false]);
  });
  it("never leaves an override ticked on another queue", () => {
    const c = normaliseConfig({ ...online, queue: "lpq", allow_graph_mismatch: true, online_update_mode: "disabled" });
    expect(c.model_id).toBeNull();
    expect(c.allow_graph_mismatch).toBe(false);
    expect(c.online_update_mode).toBe("periodic");
  });
  it("keeps the model for lpq_model and drops online options there", () => {
    const c = normaliseConfig({ ...DEFAULT_CONFIG, queue: "lpq_model", model_id: "m2", allow_width_mismatch: true });
    expect(c.model_id).toBe("m2");
    expect(c.allow_width_mismatch).toBe(false);
  });
  it("defaults every override to off", () => {
    const c = normaliseConfig(online);
    expect([c.allow_graph_mismatch, c.allow_width_mismatch, c.accept_no_provenance]).toEqual([false, false, false]);
  });
});

describe("model-unavailable notice (spec section 35)", () => {
  it("explains why lpq_model cannot run when no model matches the dataset", () => {
    const n = modelUnavailableNotice("lpq_model", undefined, 0, "osm_monaco");
    expect(n).toContain("osm_monaco");
    expect(n).toContain("lpq_model");
  });
  it("points a warm-started lpq_online at the cold start or the recorded override", () => {
    const n = modelUnavailableNotice("lpq_online", "warm", 0, "osm_monaco");
    expect(n).toMatch(/cold start/);
    expect(n).toMatch(/override/);
  });
  it("says nothing when a model exists, for a cold start, or for queues that need no model", () => {
    expect(modelUnavailableNotice("lpq_model", undefined, 1, "d")).toBeNull();
    expect(modelUnavailableNotice("lpq_online", "cold", 0, "d")).toBeNull();
    expect(modelUnavailableNotice("lpq", undefined, 0, "d")).toBeNull();
    expect(modelUnavailableNotice("binary_heap", undefined, 0, "d")).toBeNull();
  });
});
