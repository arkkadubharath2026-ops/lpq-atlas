"use client";
import Link from "next/link";
import { useEffect, useState } from "react";
import { ErrorBox, Loading } from "@/components/States";
import { api } from "@/lib/api";
import type { ResultListing } from "@/lib/types";

export default function Home() {
  const [results, setResults] = useState<ResultListing[] | null>(null);
  const [error, setError] = useState<unknown>(null);
  useEffect(() => { api.researchResults().then(setResults).catch(setError); }, []);
  return (
    <div className="page stack">
      <div>
        <h1 style={{ fontSize: 26 }}>LPQ Atlas</h1>
        <p className="muted" style={{ maxWidth: 820 }}>
          An observatory for <strong>Dynamic Approximate Shortest Path Using a Learned Priority Queue</strong>. Run Dijkstra and A* over classical
          heaps and a learned priority queue on real road and social graphs, watch the engine&apos;s own execution trace, and compare configurations
          with the project&apos;s benchmark harness. Every number comes from the C++ engine; nothing is simulated.
        </p>
        <div className="row">
          <Link className="btn" href="/explore?dataset=osm_monaco" style={{ background: "var(--primary)", color: "#04201c", fontWeight: 650 }}>Open the Route Explorer</Link>
          <Link className="btn" href="/compare">Compare classical vs LPQ</Link>
          <Link className="btn" href="/research">Research results</Link>
        </div>
      </div>
      <div className="grid cols-3">
        <div className="panel"><h3>Question</h3><p className="small muted">Can learned prioritisation save enough queue work to pay for candidate maintenance and verification?</p></div>
        <div className="panel"><h3>Guarantee</h3><p className="small muted">Exact mode matches the reference exactly; approximate mode is checked per query against ratio ≤ 1 + ε.</p></div>
        <div className="panel"><h3>Honesty</h3><p className="small muted">Traced runs are never timed; hardware counters are shown only if measured; negative results stay visible.</p></div>
      </div>
      <div className="panel">
        <h2>Stored experiments: lowest median configuration</h2>
        <p className="small muted">Read from <code>results/raw/</code>. Open the Research page for distributions, paired statistics and effect sizes.</p>
        {error ? <ErrorBox error={error} /> : !results ? <Loading /> : (
          <table className="data">
            <thead><tr><th>Experiment</th><th className="num">Records</th><th>Configurations</th><th>Lowest median</th></tr></thead>
            <tbody>
              {results.filter((r) => !r.error).map((r) => (
                <tr key={r.file}>
                  <td className="mono">{r.experiment_id}</td>
                  <td className="num">{r.records}</td>
                  <td className="small">{r.configurations.map((c) => c.replace("dijkstra/", "").replace("/exact", "")).join(", ")}</td>
                  <td className="mono small">{r.fastest_by_median}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      <footer className="credit">
        Map data © OpenStreetMap contributors (ODbL). DIMACS road networks from the 9th DIMACS Implementation Challenge; SNAP graphs from the Stanford
        Network Analysis Project.
      </footer>
    </div>
  );
}
