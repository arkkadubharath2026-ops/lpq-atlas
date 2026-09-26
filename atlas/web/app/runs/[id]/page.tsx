"use client";
import Link from "next/link";
import { use, useEffect, useMemo, useState } from "react";
import { RunViewer } from "@/components/RunViewer";
import type { PointMark } from "@/components/MapView";
import { ErrorBox, Loading, StateBadge } from "@/components/States";
import { api } from "@/lib/api";
import { useDatasetGeometry, useRun } from "@/lib/hooks";
import type { Job } from "@/lib/types";

export default function RunPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const [job, setJob] = useState<Job | null>(null);
  const [error, setError] = useState<unknown>(null);
  const run = useRun();
  const datasetId = (job?.request?.dataset_id as string | undefined) ?? null;
  const geo = useDatasetGeometry(datasetId);

  useEffect(() => {
    api.run(id).then(setJob).catch(setError);
    void run.load(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id]);

  const marks = useMemo(() => {
    const r = run.result;
    const c = geo.coords;
    if (!r) return { s: null, t: null };
    const at = (n: number): PointMark => ({ node: n, lon: c ? c[2 * n] : 0, lat: c ? c[2 * n + 1] : 0 });
    return { s: at(r.request.source), t: at(r.request.target) };
  }, [run.result, geo.coords]);

  const req = job?.request ?? {};
  const sidebar = (
    <div className="stack">
      <h1 style={{ fontSize: 16 }}>Run {id}</h1>
      {error ? <ErrorBox error={error} /> : null}
      {!job && !error ? <Loading /> : null}
      {job ? (
        <>
          <div className="row"><StateBadge state={job.state} /> <span className="small muted">{new Date(job.created_at * 1000).toLocaleString()}</span></div>
          {job.error ? <div className="notice bad small"><strong>{job.error.category}</strong>: {job.error.message}</div> : null}
          <h3>Request (as submitted)</h3>
          <pre className="mono small" style={{ whiteSpace: "pre-wrap", margin: 0 }}>{JSON.stringify(req, null, 1)}</pre>
          {datasetId ? <Link className="btn" href={`/explore?dataset=${datasetId}&run=${id}`}>Open in Route Explorer</Link> : null}
          <a className="btn" href={`/api/runs/${id}/result`} download>Download result JSON</a>
          {job.state === "COMPLETED" ? <a className="btn" href={`/api/runs/${id}/trace`} download>Download trace JSONL</a> : null}
        </>
      ) : null}
    </div>
  );
  return (
    <RunViewer
      dataset={geo.dataset}
      coords={geo.coords}
      bbox={geo.bbox}
      events={run.events}
      result={run.result}
      source={marks.s}
      target={marks.t}
      sidebar={sidebar}
      error={run.error}
      status={run.phase === "running" || run.phase === "streaming" ? <div className="notice info small">Loading run…</div> : null}
    />
  );
}
