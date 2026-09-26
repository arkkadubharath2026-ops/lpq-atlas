"use client";
import Link from "next/link";
import { useEffect, useState } from "react";
import { Empty, ErrorBox, Loading, StateBadge } from "@/components/States";
import { api } from "@/lib/api";
import type { Dataset } from "@/lib/types";
import { fmtInt } from "@/lib/format";

export default function DatasetsPage() {
  const [list, setList] = useState<Dataset[] | null>(null);
  const [error, setError] = useState<unknown>(null);
  useEffect(() => { api.datasets().then(setList).catch(setError); }, []);
  return (
    <div className="page stack">
      <h1>Datasets</h1>
      <p className="small muted">Built from the converted graphs, their metadata records and dataset configs on disk. Nothing is downloaded without an explicit request.</p>
      {error ? <ErrorBox error={error} /> : null}
      {!list && !error ? <Loading /> : null}
      {list && list.length === 0 ? <Empty title="No datasets declared" /> : null}
      {list ? (
        <div className="panel scroll-x">
          <table className="data">
            <thead><tr><th>Dataset</th><th>Family</th><th>Status</th><th className="num">Nodes</th><th className="num">Edges</th><th>Directed</th><th>Weights</th><th>Coordinates</th><th>Mode</th><th>Models</th></tr></thead>
            <tbody>
              {list.map((d) => (
                <tr key={d.id}>
                  <td><Link href={`/datasets/${d.id}`}>{d.id}</Link></td>
                  <td><span className="badge">{d.family}</span></td>
                  <td><StateBadge state={d.status} /></td>
                  <td className="num">{fmtInt(d.nodes)}</td>
                  <td className="num">{fmtInt(d.edges)}</td>
                  <td>{d.directed === undefined ? "—" : d.directed ? "yes" : "no"}</td>
                  <td className="small">{d.weights_synthesized ? "synthesized" : d.weighted === false ? "unweighted (1)" : d.weighted ? "real" : "—"}</td>
                  <td>{d.coordinate_system ?? "—"}</td>
                  <td className="small">{d.scale_mode}</td>
                  <td className="small">{d.models.join(", ") || "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : null}
    </div>
  );
}
