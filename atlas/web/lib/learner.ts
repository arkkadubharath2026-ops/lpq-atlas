import type { LpqResult } from "./types";

/**
 * How a run's online-learning status is shown (D-12).
 *
 * Attachment, recorded events, applied updates and a measured weight change are different facts, so they
 * get different states. None of them is a claim that the learner improved anything.
 */
export type LearnerState =
  | "none"
  | "attached_no_events"
  | "events_no_updates"
  | "updated"
  | "updated_unchanged"
  | "legacy_attached"
  | "legacy_none";

export interface LearnerBadge {
  state: LearnerState;
  label: string;
  /** "learned" is reserved for runs where weights were measured to change. */
  tone: "plain" | "warn" | "learned";
}

type LearnerInput = Pick<LpqResult, "online_learning" | "learner">;

const n = (v: number) => v.toLocaleString("en-US");

export function learnerBadge(l: LearnerInput): LearnerBadge {
  const s = l.learner;
  if (!s) {
    // Results written before the learner block existed carry only the old boolean, which meant "attached".
    return l.online_learning
      ? { state: "legacy_attached", label: "learner attached (older result; learning not verified)", tone: "warn" }
      : { state: "legacy_none", label: "no online learning", tone: "plain" };
  }
  if (!s.attached) return { state: "none", label: "no online learning", tone: "plain" };
  if (s.updates_applied > 0 && s.weights_changed) {
    return { state: "updated", label: `online learning: ${n(s.updates_applied)} update(s) applied, weights changed`, tone: "learned" };
  }
  if (s.updates_applied > 0) {
    return { state: "updated_unchanged", label: `${n(s.updates_applied)} update(s) applied, weights unchanged`, tone: "warn" };
  }
  if (s.events_recorded > 0) {
    return { state: "events_no_updates", label: `learner attached: ${n(s.events_recorded)} event(s) recorded, 0 updates (no learning)`, tone: "warn" };
  }
  return { state: "attached_no_events", label: "learner attached: no events recorded (no learning)", tone: "warn" };
}
