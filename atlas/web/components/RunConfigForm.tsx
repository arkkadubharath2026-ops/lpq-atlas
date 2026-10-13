"use client";
// Run configuration. The server validates every combination again; the form
// only prevents the invalid ones from being offered.
import type { Dataset, EngineConfig, ModelInfo, QueueInfo } from "@/lib/types";

export const DEFAULT_CONFIG: EngineConfig = {
  algorithm: "dijkstra",
  heuristic: "auto",
  queue: "binary_heap",
  correctness_policy: "exact",
  epsilon: 0,
  model_id: null,
  adaptive_learning: false,
  online_update_mode: "periodic",
  online_start: "warm",
  allow_graph_mismatch: false,
  allow_width_mismatch: false,
  accept_no_provenance: false,
};

/** The research LPQ variants take approximate mode; the Optimized queue is exact-only. */
const hasApproximation = (queue: string): boolean => queue.startsWith("lpq") && queue !== "lpq_optimized";

export function normaliseConfig(c: EngineConfig): EngineConfig {
  const learned = hasApproximation(c.queue);
  return {
    ...c,
    heuristic: c.algorithm === "dijkstra" ? "auto" : c.heuristic,
    correctness_policy: learned ? c.correctness_policy : "exact",
    epsilon: learned && c.correctness_policy === "approximate" ? c.epsilon || 0.1 : 0,
    model_id: c.queue === "lpq_model" || (c.queue === "lpq_online" && c.online_start !== "cold") ? c.model_id : null,
    adaptive_learning: c.queue === "lpq_adaptive" ? c.adaptive_learning : false,
    ...(c.queue === "lpq_online"
      ? {
          online_update_mode: c.online_update_mode ?? "periodic",
          online_start: c.online_start ?? "warm",
          allow_graph_mismatch: c.online_start === "cold" ? false : !!c.allow_graph_mismatch,
          allow_width_mismatch: c.online_start === "cold" ? false : !!c.allow_width_mismatch,
          accept_no_provenance: c.online_start === "cold" ? false : !!c.accept_no_provenance,
        }
      : {
          online_update_mode: "periodic" as const,
          online_start: "warm" as const,
          allow_graph_mismatch: false,
          allow_width_mismatch: false,
          accept_no_provenance: false,
        }),
  };
}

/**
 * Why a queue that needs a trained model cannot run here, or null when it can (spec section 35,
 * "model unavailable"). An empty model list with no explanation looks like a loading glitch.
 */
export function modelUnavailableNotice(
  queue: string,
  onlineStart: string | undefined,
  compatibleCount: number,
  datasetId: string | null,
): string | null {
  if (compatibleCount > 0) return null;
  const where = datasetId ?? "this dataset";
  if (queue === "lpq_model") {
    return `No trained model is registered for ${where}, so lpq_model has nothing to load. Train and export one for this dataset, or run the lpq queue (constant scorer, no model).`;
  }
  if (queue === "lpq_online" && onlineStart !== "cold") {
    return `No trained model is registered for ${where}. Choose a cold start (zero weights), or tick the override that accepts a model trained on another graph.`;
  }
  return null;
}

interface Props {
  value: EngineConfig;
  onChange: (c: EngineConfig) => void;
  dataset: Dataset | null;
  queues: QueueInfo[];
  models: ModelInfo[];
  idPrefix: string;
  disabled?: boolean;
}

export function RunConfigForm({ value, onChange, dataset, queues, models, idPrefix, disabled }: Props) {
  const set = (patch: Partial<EngineConfig>) => onChange(normaliseConfig({ ...value, ...patch }));
  const learned = hasApproximation(value.queue);
  // lpq_online accepts a model trained elsewhere only if the override is ticked (and it is recorded).
  const compatibleModels = models.filter(
    (m) => m.training_dataset === dataset?.id || (value.queue === "lpq_online" && value.allow_graph_mismatch),
  );
  const heuristics = dataset?.astar_heuristics ?? ["zero"];
  const noModel = modelUnavailableNotice(value.queue, value.online_start, compatibleModels.length, dataset?.id ?? null);
  return (
    <div className="stack">
      {noModel ? (
        <div className="notice warn small" role="status" data-testid="no-model-note">
          {noModel}
        </div>
      ) : null}
      <label className="field" htmlFor={`${idPrefix}-alg`}>
        Algorithm
        <select id={`${idPrefix}-alg`} value={value.algorithm} disabled={disabled}
          onChange={(e) => set({ algorithm: e.target.value as EngineConfig["algorithm"] })}>
          <option value="dijkstra">Dijkstra</option>
          <option value="astar">A*</option>
        </select>
      </label>
      {value.algorithm === "astar" ? (
        <label className="field" htmlFor={`${idPrefix}-h`}>
          Heuristic
          <select id={`${idPrefix}-h`} value={value.heuristic} disabled={disabled}
            onChange={(e) => set({ heuristic: e.target.value as EngineConfig["heuristic"] })}>
            {heuristics.map((h) => (
              <option key={h} value={h}>
                {h === "auto" ? "auto (from coordinates)" : h}
              </option>
            ))}
          </select>
          {!dataset?.has_coordinates ? (
            <span className="small" style={{ color: "var(--warn)" }}>
              No coordinates: only the zero heuristic is admissible (A* = Dijkstra ordering).
            </span>
          ) : null}
        </label>
      ) : null}
      <label className="field" htmlFor={`${idPrefix}-q`}>
        Priority queue
        <select id={`${idPrefix}-q`} value={value.queue} disabled={disabled}
          onChange={(e) => set({ queue: e.target.value })}>
          <optgroup label="Classical">
            {queues.filter((q) => q.family === "classical").map((q) => (
              <option key={q.id} value={q.id}>{q.name}</option>
            ))}
          </optgroup>
          <optgroup label="Learned">
            {queues.filter((q) => q.family === "learned").map((q) => (
              <option key={q.id} value={q.id} disabled={(q.requires_model && compatibleModels.length === 0) || q.engine_available === false}>
                {q.name}
                {q.requires_model && compatibleModels.length === 0 ? " — no model for this dataset" : ""}
                {q.engine_available === false ? " — not in this engine build" : ""}
              </option>
            ))}
          </optgroup>
        </select>
      </label>
      {value.queue === "lpq_online" ? (
        <fieldset className="stack" style={{ border: 0, padding: 0, margin: 0 }} disabled={disabled}>
          <div className="seg" role="group" aria-label="Starting model">
            {(["warm", "cold"] as const).map((s) => (
              <button key={s} type="button" aria-pressed={value.online_start === s} onClick={() => set({ online_start: s })}>
                {s === "warm" ? "warm start (choose a model)" : "cold start (zero weights)"}
              </button>
            ))}
          </div>
          {value.online_start !== "cold" ? (
            <label className="field" htmlFor={`${idPrefix}-m`}>
              Starting model (explicit; there is no default)
              <select id={`${idPrefix}-m`} value={value.model_id ?? ""} onChange={(e) => set({ model_id: e.target.value || null })}>
                <option value="">— choose —</option>
                {compatibleModels.map((m) => (
                  <option key={m.id} value={m.id}>{m.id} ({m.kind}, trained on {m.training_dataset ?? "unknown"})</option>
                ))}
              </select>
            </label>
          ) : null}
          <label className="field" htmlFor={`${idPrefix}-um`}>
            Update mode
            <select id={`${idPrefix}-um`} value={value.online_update_mode ?? "periodic"}
              onChange={(e) => set({ online_update_mode: e.target.value as EngineConfig["online_update_mode"] })}>
              <option value="periodic">periodic (applied after the search)</option>
              <option value="every_event">every event (applied after the search)</option>
              <option value="adaptive">adaptive (only if recent error is high)</option>
              <option value="disabled">disabled (record only: the frozen control)</option>
            </select>
          </label>
          <p className="small muted">
            Updates are applied after the search, so this search runs on the starting model. An update is not an
            improvement, and nothing on this page measures whether the model got better.
          </p>
          {value.online_start !== "cold" ? (
            <details>
              <summary className="small">Model checks you can override (each is recorded in the result)</summary>
              <label className="check"><input type="checkbox" checked={!!value.allow_graph_mismatch}
                onChange={(e) => set({ allow_graph_mismatch: e.target.checked })} />Accept a model trained on another graph</label>
              <label className="check"><input type="checkbox" checked={!!value.allow_width_mismatch}
                onChange={(e) => set({ allow_width_mismatch: e.target.checked })} />Accept a model trained for another bucket width</label>
              <label className="check"><input type="checkbox" checked={!!value.accept_no_provenance}
                onChange={(e) => set({ accept_no_provenance: e.target.checked })} />Accept a model with no report</label>
            </details>
          ) : null}
        </fieldset>
      ) : null}
      {value.queue === "lpq_model" ? (
        <label className="field" htmlFor={`${idPrefix}-m`}>
          Model (trained on this dataset)
          <select id={`${idPrefix}-m`} value={value.model_id ?? ""} disabled={disabled}
            onChange={(e) => set({ model_id: e.target.value || null })}>
            <option value="">— choose —</option>
            {compatibleModels.map((m) => (
              <option key={m.id} value={m.id}>{m.id} ({m.kind}, split {m.split_status})</option>
            ))}
          </select>
        </label>
      ) : null}
      {value.queue === "lpq_adaptive" ? (
        <label className="check">
          <input type="checkbox" checked={value.adaptive_learning} disabled={disabled}
            onChange={(e) => set({ adaptive_learning: e.target.checked })} />
          Also update the policy model online
        </label>
      ) : null}
      <fieldset style={{ border: 0, padding: 0, margin: 0 }} disabled={disabled || !learned}>
        <legend className="small muted" style={{ marginBottom: 4 }}>
          Correctness policy {learned ? "" : "(exact only: this queue has no approximation)"}
        </legend>
        <div className="seg" role="group">
          {(["exact", "approximate"] as const).map((p) => (
            <button key={p} type="button" aria-pressed={value.correctness_policy === p}
              onClick={() => set({ correctness_policy: p })}>
              {p}
            </button>
          ))}
        </div>
        {value.correctness_policy === "approximate" ? (
          <label className="field" htmlFor={`${idPrefix}-eps`} style={{ marginTop: 8 }}>
            ε (returned ≤ (1+ε) × optimal)
            <input id={`${idPrefix}-eps`} type="number" min={0.01} max={1} step={0.01} value={value.epsilon}
              onChange={(e) => set({ epsilon: Math.min(1, Math.max(0.01, Number(e.target.value) || 0.01)) })} />
          </label>
        ) : null}
      </fieldset>
    </div>
  );
}
