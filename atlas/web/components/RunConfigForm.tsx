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
};

export function normaliseConfig(c: EngineConfig): EngineConfig {
  const learned = c.queue.startsWith("lpq");
  return {
    ...c,
    heuristic: c.algorithm === "dijkstra" ? "auto" : c.heuristic,
    correctness_policy: learned ? c.correctness_policy : "exact",
    epsilon: learned && c.correctness_policy === "approximate" ? c.epsilon || 0.1 : 0,
    model_id: c.queue === "lpq_model" ? c.model_id : null,
    adaptive_learning: c.queue === "lpq_adaptive" ? c.adaptive_learning : false,
  };
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
  const learned = value.queue.startsWith("lpq");
  const compatibleModels = models.filter((m) => m.training_dataset === dataset?.id);
  const heuristics = dataset?.astar_heuristics ?? ["zero"];
  return (
    <div className="stack">
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
              <option key={q.id} value={q.id} disabled={q.requires_model && compatibleModels.length === 0}>
                {q.name}
                {q.requires_model && compatibleModels.length === 0 ? " — no model for this dataset" : ""}
              </option>
            ))}
          </optgroup>
        </select>
      </label>
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
          Correctness policy {learned ? "" : "(exact only: classical queues have no approximation)"}
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
