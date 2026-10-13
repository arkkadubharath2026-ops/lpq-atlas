"use client";
import Link from "next/link";
import { use, useEffect, useState } from "react";
import { Bars } from "@/components/Charts";
import { ErrorBox, Loading } from "@/components/States";
import { api } from "@/lib/api";
import type { ModelInfo, TrainingManifestProvenance } from "@/lib/types";
import { fmtNum } from "@/lib/format";

const FEATURE_DOC: Record<string, string> = {
  out_degree: "Out-degree, normalised by the graph's maximum degree",
  min_adjacent_weight: "Smallest outgoing arc weight / graph max weight",
  mean_adjacent_weight: "Mean outgoing arc weight / graph max weight",
  max_adjacent_weight: "Largest outgoing arc weight / graph max weight",
  adjacent_weight_spread: "(max − min) outgoing weight / graph max weight",
  degree_relative_to_mean: "Out-degree / mean out-degree",
};

/** Says how the training manifest was identified, and whether that can be trusted. */
function provenanceText(p: TrainingManifestProvenance | undefined): string {
  if (!p) return "not reported (older API)";
  switch (p.status) {
    case "verified":
      return `recorded in the model report; SHA-256 verified${p.selection ? ` (${p.selection})` : ""}`;
    case "hash_mismatch":
      return "recorded in the model report, but the file has changed since training — not trusted";
    case "manifest_missing":
      return "recorded in the model report, but that file is not in the repository";
    default:
      return p.source === "inferred_from_dataset_filename"
        ? "not recorded in this (older) report; inferred from the dataset name, so it may not be the manifest used"
        : "not recorded in this report";
  }
}

export default function ModelInspector({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const [m, setM] = useState<ModelInfo | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [nodesText, setNodesText] = useState("0,1,2");
  const [scores, setScores] = useState<Record<string, Record<string, number>> | null>(null);
  const [scoreError, setScoreError] = useState<unknown>(null);
  const [scoring, setScoring] = useState(false);
  useEffect(() => { api.model(id).then(setM).catch(setError); }, [id]);

  const score = async () => {
    const nodes = nodesText.split(",").map((s) => Number(s.trim())).filter((n) => Number.isInteger(n) && n >= 0).slice(0, 50);
    if (!nodes.length) return;
    setScoring(true);
    setScoreError(null);
    try { setScores((await api.modelNodes(id, nodes)).nodes); } catch (e) { setScoreError(e); } finally { setScoring(false); }
  };

  if (error) return <div className="page"><ErrorBox error={error} /></div>;
  if (!m) return <div className="page"><Loading /></div>;
  const rep = m.report as Record<string, number | string | Record<string, number>>;
  const weights = Object.entries(m.native_weights ?? {}).filter(([, v]) => v !== 0);
  return (
    <div className="page stack">
      <div className="row"><Link href="/models">← Models</Link></div>
      <h1>{m.id}</h1>
      <div className="grid cols-3">
        <div className="panel">
          <h3>Artifact</h3>
          <dl className="kv small">
            <dt>Kind</dt><dd>{m.kind}</dd>
            <dt>File</dt><dd>{m.file}</dd>
            <dt>Size</dt><dd>{m.size_bytes} B</dd>
            <dt>SHA-256</dt><dd style={{ wordBreak: "break-all" }}>{m.sha256}</dd>
            <dt>Feature layout</dt><dd>v{m.feature_layout_version} · {m.feature_count} slots</dd>
            <dt>C++ loader</dt><dd>{m.loader_ok ? "loads" : "FAILED"}</dd>
          </dl>
        </div>
        <div className="panel">
          <h3>Mode</h3>
          <span className="badge learned">{m.mode.replace(/_/g, " ")}</span>
          <p className="small" style={{ marginTop: 8 }}>{m.mode_explanation}</p>
        </div>
        <div className="panel">
          <h3>Training provenance</h3>
          <dl className="kv small">
            <dt>Dataset</dt><dd>{m.training_dataset}</dd>
            <dt>Training queries</dt><dd>{m.training_manifest ?? "not found"}</dd>
            <dt>Manifest provenance</dt><dd>{provenanceText(m.training_manifest_provenance)}</dd>
            <dt>Query seed / count</dt><dd>{String(rep.query_seed ?? "?")} / {String(rep.query_count ?? "?")}</dd>
            <dt>Training seed</dt><dd>{String(rep.seed ?? "?")}</dd>
            <dt>L2 inverse strength</dt><dd>{String(rep.l2_inverse_strength ?? "?")}</dd>
            <dt>Test fraction</dt><dd>{String(rep.test_fraction ?? "?")}</dd>
          </dl>
        </div>
      </div>

      <div className="panel">
        <h2>Training / evaluation split</h2>
        <div className="row" style={{ marginBottom: 8 }}>
          <span className={`badge ${m.split_status === "DISJOINT" ? "ok" : m.split_status === "OVERLAPPING" ? "bad" : "warn"}`}>Training/Evaluation split: {m.split_status}</span>
          <span className="small muted">Checked automatically against every evaluation manifest of the dataset.</span>
        </div>
        <table className="data">
          <thead><tr><th>Evaluation manifest</th><th>Status</th><th className="num">Train q</th><th className="num">Eval q</th><th className="num">Shared pairs</th><th className="num">Shared endpoints</th><th className="num">Shared sources</th><th>Rule</th></tr></thead>
          <tbody>
            {m.splits.map((s) => (
              <tr key={s.evaluation_manifest}>
                <td className="mono">{s.evaluation_manifest}</td>
                <td><span className={`badge ${s.status === "DISJOINT" ? "ok" : s.status === "OVERLAPPING" ? "bad" : "warn"}`}>{s.status}</span></td>
                <td className="num">{s.training_queries}</td><td className="num">{s.evaluation_queries}</td>
                <td className="num">{s.shared_pairs}</td><td className="num">{s.shared_endpoints}</td>
                <td className="num">{s.shared_sources ?? "?"}</td><td>{s.level ?? "pair"}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="grid cols-2">
        <div className="panel">
          <h2>Predictive quality (held-out rows)</h2>
          <div className="notice warn small" style={{ marginBottom: 8 }}>
            These are classification metrics of the scorer. They are <strong>not</strong> a measure of search speed — see the Research page for
            end-to-end runtime, where these models have not produced a speedup.
          </div>
          <dl className="kv small">
            <dt>ROC-AUC</dt><dd>{fmtNum(rep.roc_auc as number, 4)}</dd>
            <dt>Average precision</dt><dd>{fmtNum(rep.average_precision as number, 4)} (baseline {fmtNum(rep.average_precision_baseline as number, 4)})</dd>
            <dt>Accuracy</dt><dd>{fmtNum(rep.accuracy as number, 4)} (majority baseline {fmtNum(rep.majority_baseline_accuracy as number, 4)})</dd>
            <dt>Positive rate</dt><dd>{fmtNum(rep.positive_rate as number, 4)}</dd>
            <dt>Observations</dt><dd>{fmtNum(rep.observations as number, 0)}</dd>
          </dl>
        </div>
        <div className="panel">
          <h2>Weights (read back by the C++ loader)</h2>
          <p className="small muted">A linear model&apos;s coefficients. They are its parameters, not a feature-importance explanation.</p>
          <Bars label="Model weights" items={weights.map(([k, v]) => ({ key: k, value: Math.abs(v) }))} format={(v) => fmtNum(v, 3)} width={560} />
          <table className="data">
            <tbody>{weights.map(([k, v]) => <tr key={k}><td className="mono">{k}</td><td className="num">{fmtNum(v, 6)}</td><td className="small dim">{FEATURE_DOC[k] ?? ""}</td></tr>)}</tbody>
          </table>
        </div>
      </div>

      <div className="panel stack">
        <h2>Node inspector</h2>
        <p className="small muted">Static features and the score the learned queue assigns, computed by the engine&apos;s own <code>GraphFeatures</code> and <code>StaticNodeScorer</code> on {m.training_dataset}.</p>
        <div className="row">
          <label className="field" style={{ width: 320 }}>Node ids (comma-separated, up to 50)
            <input type="text" value={nodesText} onChange={(e) => setNodesText(e.target.value)} />
          </label>
          <button type="button" onClick={score} disabled={scoring}>{scoring ? "Scoring…" : "Score"}</button>
        </div>
        {scoreError ? <ErrorBox error={scoreError} /> : null}
        {scores ? (
          <div className="scroll-x">
            <table className="data">
              <thead><tr><th>Node</th>{Object.keys(FEATURE_DOC).map((f) => <th key={f} className="num">{f}</th>)}<th className="num">score</th></tr></thead>
              <tbody>
                {Object.entries(scores).map(([n, v]) => (
                  <tr key={n}><td className="mono">{n}</td>{Object.keys(FEATURE_DOC).map((f) => <td key={f} className="num">{fmtNum(v[f], 5)}</td>)}<td className="num" style={{ color: "var(--learned)" }}>{fmtNum(v.score, 5)}</td></tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : null}
      </div>
    </div>
  );
}
