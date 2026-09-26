"use client";
import Link from "next/link";
import { Fragment, use, useEffect, useState } from "react";
import { ErrorBox, Loading, StateBadge } from "@/components/States";
import { api, waitForJob } from "@/lib/api";
import type { Dataset, Job } from "@/lib/types";
import { fmtInt, fmtNum } from "@/lib/format";

export default function DatasetPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const [d, setD] = useState<Dataset | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [prep, setPrep] = useState<Job | null>(null);
  const load = () => api.dataset(id).then(setD).catch(setError);
  useEffect(() => { void load(); // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id]);

  const prepare = async () => {
    if (!confirm(`Prepare ${id}? This downloads the source files listed below if they are not present and converts them.`)) return;
    try {
      const j = await api.prepare(id);
      setPrep(j);
      await waitForJob(() => api.preparation(j.id), setPrep, 800);
      await load();
    } catch (e) { setError(e); }
  };

  if (error) return <div className="page"><ErrorBox error={error} /></div>;
  if (!d) return <div className="page"><Loading /></div>;
  return (
    <div className="page stack">
      <div className="row"><Link href="/datasets">← Datasets</Link></div>
      <div className="row"><h1 style={{ margin: 0 }}>{d.id}</h1><span className="badge">{d.family}</span><StateBadge state={d.status} /></div>
      {d.description ? <p className="muted">{d.description}</p> : null}
      {d.status_detail ? <div className="notice bad small">{d.status_detail}</div> : null}
      {d.status !== "READY" ? (
        <div className="notice warn row">
          <span>This dataset is not prepared.</span>
          <button type="button" onClick={prepare} disabled={Boolean(prep && !["COMPLETED", "FAILED"].includes(prep.state))}>Prepare dataset</button>
          {prep ? <span className="small">{prep.state} {prep.progress ?? ""} {prep.error?.message ?? ""}</span> : null}
        </div>
      ) : (
        <div className="row"><Link className="btn" href={`/explore?dataset=${d.id}`}>Open in Route Explorer</Link><Link className="btn" href="/benchmarks">Benchmark it</Link></div>
      )}
      <div className="grid cols-3">
        <div className="panel">
          <h3>Graph</h3>
          <dl className="kv small">
            <dt>Nodes</dt><dd>{fmtInt(d.nodes)}</dd>
            <dt>Edges / arcs</dt><dd>{fmtInt(d.edges)} / {fmtInt(d.arcs)}</dd>
            <dt>Directed</dt><dd>{d.directed ? "yes" : "no"}</dd>
            <dt>Weights</dt><dd>{d.weights_synthesized ? "synthesized" : d.weighted ? "real" : "unit"}</dd>
            <dt>Weight range</dt><dd>{fmtNum(d.min_weight)} – {fmtNum(d.max_weight)} (mean {fmtNum(d.mean_weight, 1)})</dd>
            <dt>Coordinates</dt><dd>{d.coordinate_system}</dd>
            <dt>Checksum (FNV-1a arcs)</dt><dd>{d.checksum_fnv1a64}</dd>
            <dt>Scale mode</dt><dd>{d.scale_mode}</dd>
          </dl>
        </div>
        <div className="panel">
          <h3>Algorithms</h3>
          <dl className="kv small">
            <dt>Compatible</dt><dd>{d.compatible_algorithms.join(", ")}</dd>
            <dt>A* heuristics</dt><dd>{d.astar_heuristics.join(", ")}</dd>
            <dt>Trained models</dt><dd>{d.models.join(", ") || "none"}</dd>
          </dl>
          {d.normalization ? (<><h3 style={{ marginTop: 10 }}>Normalisation</h3><dl className="kv small">{Object.entries(d.normalization).map(([k, v]) => <Fragment key={k}><dt>{k}</dt><dd>{v}</dd></Fragment>)}</dl></>) : null}
        </div>
        <div className="panel">
          <h3>Provenance</h3>
          <dl className="kv small">
            <dt>Format</dt><dd>{d.source_format ?? "—"}</dd>
            {Object.entries(d.provenance ?? {}).map(([k, v]) => (<Fragment key={k}><dt>{k}</dt><dd style={{ wordBreak: "break-word" }}>{String(v)}</dd></Fragment>))}
          </dl>
          {(d.source_urls ?? []).map((u) => <div key={u} className="small mono" style={{ wordBreak: "break-all" }}>{u}</div>)}
        </div>
      </div>
      {d.osm ? (
        <div className="panel">
          <h2>OpenStreetMap extraction</h2>
          <div className="grid cols-2">
            <dl className="kv small">
              <dt>Region</dt><dd>{d.osm.region}</dd>
              <dt>Snapshot</dt><dd>{d.osm.snapshot_date}</dd>
              <dt>Source SHA-256</dt><dd style={{ wordBreak: "break-all" }}>{d.osm.source_sha256}</dd>
              <dt>Licence</dt><dd>{d.osm.licence}</dd>
              <dt>Road policy</dt><dd>v{d.osm.road_policy_version} ({d.osm.extractor})</dd>
              <dt>Weight unit</dt><dd>{d.osm.weight_unit} (great-circle length: shortest, not fastest)</dd>
              <dt>Turn restrictions</dt><dd>{d.osm.turn_restrictions_modelled ? "modelled" : "NOT modelled"}</dd>
              <dt>Ways kept</dt><dd>{fmtInt(d.osm.ways_kept)}</dd>
              <dt>Nodes outside largest SCC (dropped)</dt><dd>{fmtInt(d.osm.nodes_dropped_outside_largest_scc)}</dd>
            </dl>
            <div>
              <h3>Ways excluded by policy</h3>
              <table className="data"><tbody>
                {Object.entries(d.osm.ways_excluded ?? {}).sort((a, b) => b[1] - a[1]).map(([k, v]) => <tr key={k}><td className="mono">{k}</td><td className="num">{fmtInt(v)}</td></tr>)}
              </tbody></table>
            </div>
          </div>
        </div>
      ) : null}
      <div className="panel">
        <h2>Query manifests</h2>
        <table className="data">
          <thead><tr><th>File</th><th>Role</th><th className="num">Seed</th><th className="num">Queries</th></tr></thead>
          <tbody>{d.query_manifests.map((m) => <tr key={m.file}><td className="mono">{m.file}</td><td><span className={`badge ${m.role === "training" ? "learned" : "info"}`}>{m.role}</span></td><td className="num">{m.seed}</td><td className="num">{fmtInt(m.query_count)}</td></tr>)}</tbody>
        </table>
      </div>
    </div>
  );
}
