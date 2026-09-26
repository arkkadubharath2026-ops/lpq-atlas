"use client";
import Link from "next/link";
import { useEffect, useState } from "react";
import { Empty, ErrorBox, Loading } from "@/components/States";
import { api } from "@/lib/api";
import type { ModelInfo } from "@/lib/types";

// Implementations present in cpp/include/lpq/models/. Listing them is a fact
// about the code; which of them is TRAINED, ACTIVE or BENCHMARKED is separate
// and shown separately (spec D-04).
const IMPLEMENTATIONS = [
  ["heuristic_predictor.hpp", "Heuristic predictor", "No trained artifact in models/exported"],
  ["linear_predictor.hpp", "Linear predictor", "No trained artifact in models/exported"],
  ["logistic_predictor.hpp", "Logistic predictor", "Trained artifacts below; the only family lpq_model loads"],
  ["mlp_predictor.hpp", "MLP predictor", "Implemented and unit-tested; not trained or used by any benchmarked configuration"],
  ["tree_ensemble.hpp", "Tree ensemble", "Implemented and unit-tested; not trained or used by any benchmarked configuration"],
  ["calibration.hpp", "Calibration", "Feeds the logistic model's confidence output; the queue stores the uncalibrated logit (score), so calibration does not affect the benchmarked path"],
];

export default function ModelsPage() {
  const [models, setModels] = useState<ModelInfo[] | null>(null);
  const [error, setError] = useState<unknown>(null);
  useEffect(() => { api.models().then(setModels).catch(setError); }, []);
  return (
    <div className="page stack">
      <h1>Models</h1>
      <div className="notice info small">
        In every learned configuration the model is used as a <strong>static scorer</strong>: one score per node is precomputed from static graph features
        before the search, and the queue reads it at push time. No model is updated during a search. The adaptive configuration adapts the queue&apos;s
        <em> policy</em> (and optionally an online policy model), which is distinct from these artifacts.
      </div>
      {error ? <ErrorBox error={error} /> : null}
      <div className="panel">
        <h2>Trained artifacts</h2>
        {!models ? <Loading /> : models.length === 0 ? <Empty title="No trained models in models/exported" /> : (
          <table className="data">
            <thead><tr><th>Model</th><th>Kind</th><th>Trained on</th><th>Split</th><th>Active in</th><th>Loader</th><th>SHA-256</th></tr></thead>
            <tbody>
              {models.map((m) => (
                <tr key={m.id}>
                  <td><Link href={`/models/${m.id}`}>{m.id}</Link></td>
                  <td>{m.kind ?? "?"}</td>
                  <td>{m.training_dataset ?? "unknown"}</td>
                  <td><span className={`badge ${m.split_status === "DISJOINT" ? "ok" : m.split_status === "OVERLAPPING" ? "bad" : "warn"}`}>{m.split_status}</span></td>
                  <td className="small">queue <code>lpq_model</code> on {m.training_dataset}</td>
                  <td>{m.loader_ok ? "C++ loader OK" : "loader failed"}</td>
                  <td className="mono small">{m.sha256.slice(0, 16)}…</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="small dim">Two artifacts with the same SHA-256 are byte-identical files, whatever their names.</p>
      </div>
      <div className="panel">
        <h2>Model implementations in the engine</h2>
        <table className="data">
          <thead><tr><th>Source</th><th>Family</th><th>Status</th></tr></thead>
          <tbody>{IMPLEMENTATIONS.map(([f, n, s]) => <tr key={f}><td className="mono">{f}</td><td>{n}</td><td className="small">{s}</td></tr>)}</tbody>
        </table>
      </div>
    </div>
  );
}
