import { describe, expect, it } from "vitest";
import { learnerBadge } from "./learner";
import type { LearnerStatus } from "./types";

const base: LearnerStatus = {
  schema: 1, attached: true, events_recorded: 0, events_applied: 0, events_dropped: 0, updates_applied: 0, weights_changed: false,
};

describe("learner status badge", () => {
  it("shows no learning when no learner was attached", () => {
    expect(learnerBadge({ online_learning: false, learner: { ...base, attached: false } }).state).toBe("none");
  });
  it("does not call an attached learner with no events 'online learning'", () => {
    const b = learnerBadge({ online_learning: false, learner: base });
    expect(b.state).toBe("attached_no_events");
    expect(b.tone).not.toBe("learned");
  });
  it("separates recorded events from applied updates", () => {
    const b = learnerBadge({ online_learning: false, learner: { ...base, events_recorded: 1234 } });
    expect(b.state).toBe("events_no_updates");
    expect(b.label).toContain("1,234");
    expect(b.label).toContain("0 updates");
    expect(b.tone).toBe("warn");
  });
  it("calls it online learning only when updates were applied AND the weights changed", () => {
    const changed = learnerBadge({ online_learning: true, learner: { ...base, events_recorded: 9, updates_applied: 2, weights_changed: true } });
    expect(changed.state).toBe("updated");
    expect(changed.tone).toBe("learned");
    const same = learnerBadge({ online_learning: false, learner: { ...base, events_recorded: 9, updates_applied: 2, weights_changed: false } });
    expect(same.state).toBe("updated_unchanged");
    expect(same.tone).not.toBe("learned");
  });
  it("does not trust the legacy boolean, which meant only 'attached'", () => {
    const old = learnerBadge({ online_learning: true });
    expect(old.state).toBe("legacy_attached");
    expect(old.tone).not.toBe("learned");
    expect(old.label).toContain("not verified");
    expect(learnerBadge({ online_learning: false }).state).toBe("legacy_none");
  });
  it("never words a state as an improvement", () => {
    for (const learner of [undefined, base, { ...base, events_recorded: 5 }, { ...base, updates_applied: 1, weights_changed: true }]) {
      expect(learnerBadge({ online_learning: false, learner }).label).not.toMatch(/improv|better|faster|help/i);
    }
  });
});
