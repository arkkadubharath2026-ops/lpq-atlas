"use client";
import { useEffect, useState } from "react";
import { ResultView } from "@/components/ResultView";
import { Empty, ErrorBox, Loading, StateBadge } from "@/components/States";
import { api, waitForJob } from "@/lib/api";
import type { Dataset, Job, ModelInfo, QueueInfo, ResultSummary } from "@/lib/types";
import { fmtInt, queueLabel } from "@/lib/format";

export default function BenchmarksPage() {
  const [datasets, setDatasets] = useState<Dataset[] | null>(null);
  const [queues, setQueues] = useState<QueueInfo[]>([]);
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [history, setHistory] = useState<Job[]>([]);
  const [error, setError] = useState<unknown>(null);
  const [ds, setDs] = useState("osm_monaco");
  const [algorithm, setAlgorithm] = useState<"dijkstra" | "astar">("dijkstra");
  const [heuristic, setHeuristic] = useState("haversine");
  const [selected, setSelected] = useState<string[]>(["binary_heap", "radix_heap", "lpq"]);
  const [policy, setPolicy] = useState<"exact" | "approximate">("exact");
  const [epsilon, setEpsilon] = useState(0.1);
  const [modelId, setModelId] = useState<string>("");
  const [manifest, setManifest] = useState<string>("");
  const [count, setCount] = useState(20);
  const [seed, setSeed] = useState(42);
  const [reps, setReps] = useState(7);
  const [warmup, setWarmup] = useState(1);
  const [baseline, setBaseline] = useState("binary_heap");
  const [job, setJob] = useState<Job | null>(null);
  const [summary, setSummary] = useState<ResultSummary | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    Promise.all([api.datasets(), api.queues(), api.models(), api.benchmarks()])
      .then(([d, q, m, h]) => { setDatasets(d); setQueues(q); setModels(m); setHistory(h); })
      .catch(setError);
  }, []);

  const dataset = datasets?.find((d) => d.id === ds) ?? null;
  const dsModels = models.filter((m) => m.training_dataset === ds);
  const toggle = (q: string) => setSelected((s) => (s.includes(q) ? s.filter((x) => x !== q) : [...s, q]));

  const open = async (id: string) => {
    setError(null);
    setSummary(null);
    try {
      const j = await api.benchmark(id);
      setJob(j);
      if (j.state === "COMPLETED") setSummary(await api.benchmarkResults(id));
    } catch (e) { setError(e); }
  };

  const submit = async () => {
    setBusy(true);
    setError(null);
    setSummary(null);
    try {
      const j = await api.createBenchmark({
        dataset_id: ds, algorithm, heuristic, queues: selected, correctness_policy: policy,
        epsilon: policy === "approximate" ? epsilon : 0, model_id: selected.includes("lpq_model") ? modelId || null : null,
        query_manifest: manifest || null, query_count: count, seed, repetitions: reps, warmup,
        baseline: selected.includes(baseline) ? baseline : null,
      });
      setJob(j);
      const done = await waitForJob(() => api.benchmark(j.id), setJob, 800);
      setHistory(await api.benchmarks());
      if (done.state === "COMPLETED") setSummary(await api.benchmarkResults(j.id));
      else throw new Error(done.error?.message ?? `Benchmark ${done.state.toLowerCase()}`);
    } catch (e) { setError(e); } finally { setBusy(false); }
  };

  return (
    <div className="page stack">
      <h1>Benchmark workspace</h1>
      <p className="muted small">
        Runs the project&apos;s own <code>lpq_benchmark</code> harness: one pinned process for the whole matrix, identical paired queries for every queue,
        every query validated against the trusted reference before timing, tracing off. Results are the harness&apos;s raw records.
      </p>
      {error ? <ErrorBox error={error} /> : null}
      {!datasets && !error ? <Loading /> : null}
      {datasets ? (
        <div className="grid cols-3">
          <div className="panel stack">
            <h3>Dataset &amp; queries</h3>
            <label className="field">Dataset
              <select value={ds} onChange={(e) => { setDs(e.target.value); setManifest(""); setModelId(""); }}>
                {datasets.map((d) => <option key={d.id} value={d.id} disabled={d.status !== "READY"}>{d.id} ({d.nodes ? fmtInt(d.nodes) : "?"} nodes)</option>)}
              </select>
            </label>
            <label className="field">Query set
              <select value={manifest} onChange={(e) => setManifest(e.target.value)}>
                <option value="">generate from seed</option>
                {dataset?.query_manifests.filter((m) => m.role === "evaluation").map((m) => (
                  <option key={m.file} value={m.file}>{m.file} (seed {m.seed}, {m.query_count})</option>
                ))}
              </select>
            </label>
            <div className="grid cols-2">
              <label className="field">Queries<input type="number" min={1} max={1000} value={count} onChange={(e) => setCount(Math.max(1, Math.min(1000, Number(e.target.value) || 1)))} /></label>
              <label className="field">Seed<input type="number" min={0} value={seed} disabled={Boolean(manifest)} onChange={(e) => setSeed(Math.max(0, Number(e.target.value) || 0))} /></label>
              <label className="field">Repetitions<input type="number" min={1} max={51} value={reps} onChange={(e) => setReps(Math.max(1, Math.min(51, Number(e.target.value) || 1)))} /></label>
              <label className="field">Warm-up<input type="number" min={0} max={10} value={warmup} onChange={(e) => setWarmup(Math.max(0, Math.min(10, Number(e.target.value) || 0)))} /></label>
            </div>
          </div>
          <div className="panel stack">
            <h3>Algorithm &amp; policy</h3>
            <label className="field">Algorithm
              <select value={algorithm} onChange={(e) => setAlgorithm(e.target.value as "dijkstra" | "astar")}>
                <option value="dijkstra">Dijkstra</option><option value="astar">A*</option>
              </select>
            </label>
            {algorithm === "astar" ? (
              <label className="field">Heuristic
                <select value={heuristic} onChange={(e) => setHeuristic(e.target.value)}>
                  {(dataset?.coordinate_system === "geographic" ? ["haversine", "zero"] : dataset?.coordinate_system === "euclidean" ? ["euclidean", "zero"] : ["zero"]).map((h) => <option key={h}>{h}</option>)}
                </select>
              </label>
            ) : null}
            <label className="field">Correctness policy
              <select value={policy} onChange={(e) => setPolicy(e.target.value as "exact" | "approximate")}>
                <option value="exact">exact</option><option value="approximate">approximate (LPQ only; others reported as not measured)</option>
              </select>
            </label>
            {policy === "approximate" ? <label className="field">ε<input type="number" min={0.01} max={1} step={0.01} value={epsilon} onChange={(e) => setEpsilon(Number(e.target.value) || 0.01)} /></label> : null}
          </div>
          <div className="panel stack">
            <h3>Queues</h3>
            {queues.map((q) => (
              <label key={q.id} className="check">
                <input type="checkbox" checked={selected.includes(q.id)} onChange={() => toggle(q.id)}
                  disabled={q.requires_model && dsModels.length === 0} />
                {q.name}{q.requires_model && dsModels.length === 0 ? <span className="dim small"> (no model for this dataset)</span> : null}
              </label>
            ))}
            {selected.includes("lpq_model") ? (
              <label className="field">Model
                <select value={modelId} onChange={(e) => setModelId(e.target.value)}>
                  <option value="">— choose —</option>
                  {dsModels.map((m) => <option key={m.id} value={m.id}>{m.id}</option>)}
                </select>
              </label>
            ) : null}
            <label className="field">Baseline for paired statistics
              <select value={baseline} onChange={(e) => setBaseline(e.target.value)}>
                {selected.map((q) => <option key={q} value={q}>{queueLabel(q)}</option>)}
              </select>
            </label>
            <button type="button" className="primary" disabled={busy || selected.length === 0 || (selected.includes("lpq_model") && !modelId)} onClick={submit}>
              {busy ? "Running…" : "Run benchmark"}
            </button>
            {job ? (
              <div className="small row">
                {job.id} <StateBadge state={job.state} /> {job.progress ? <span className="dim">{job.progress}</span> : null}
                {busy ? <button type="button" className="danger" onClick={() => api.cancelBenchmark(job.id)}>Cancel</button> : null}
              </div>
            ) : null}
          </div>
        </div>
      ) : null}

      {job?.state === "COMPLETED" && summary ? (
        <>
          <div className="row"><h2 style={{ margin: 0 }}>Results</h2><a className="btn" href={`/api/benchmarks/${job.id}/results?raw=true`} download>Export raw records (JSON)</a></div>
          <ResultView s={summary} />
        </>
      ) : job && job.state === "COMPLETED" && !summary ? <Loading /> : !job ? <Empty title="No benchmark selected">Run one above or open a previous one below.</Empty> : null}

      <div className="panel">
        <h2>History</h2>
        {history.length === 0 ? <p className="small dim">No benchmarks have been run from the workspace yet.</p> : (
          <table className="data">
            <thead><tr><th>Id</th><th>State</th><th>Dataset</th><th>Queues</th><th>Created</th></tr></thead>
            <tbody>
              {history.map((h) => (
                <tr key={h.id} style={{ cursor: "pointer" }} onClick={() => open(h.id)}>
                  <td className="mono"><a href="#" onClick={(e) => { e.preventDefault(); void open(h.id); }}>{h.id}</a></td>
                  <td><StateBadge state={h.state} /></td>
                  <td>{String(h.summary.dataset_id ?? "")}</td>
                  <td className="small">{((h.summary.queues as string[]) ?? []).join(", ")}</td>
                  <td className="small">{new Date(h.created_at * 1000).toLocaleString()}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </div>
  );
}
