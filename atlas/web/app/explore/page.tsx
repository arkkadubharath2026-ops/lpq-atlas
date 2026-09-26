"use client";
import Link from "next/link";
import { Suspense, useEffect, useState } from "react";
import { useRouter, useSearchParams } from "next/navigation";
import { RunConfigForm, DEFAULT_CONFIG, normaliseConfig } from "@/components/RunConfigForm";
import { RunViewer } from "@/components/RunViewer";
import type { PointMark } from "@/components/MapView";
import { ErrorBox, Loading, StateBadge } from "@/components/States";
import { api } from "@/lib/api";
import { useDatasetGeometry, useRun } from "@/lib/hooks";
import type { Dataset, EngineConfig, ModelInfo, QueueInfo } from "@/lib/types";
import { fmtInt, fmtNum } from "@/lib/format";

function ExploreInner() {
  const params = useSearchParams();
  const router = useRouter();
  const [datasets, setDatasets] = useState<Dataset[] | null>(null);
  const [queues, setQueues] = useState<QueueInfo[]>([]);
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [metaError, setMetaError] = useState<unknown>(null);
  const [datasetId, setDatasetId] = useState<string | null>(params.get("dataset"));
  const [config, setConfig] = useState<EngineConfig>(DEFAULT_CONFIG);
  const [source, setSource] = useState<PointMark | null>(null);
  const [target, setTarget] = useState<PointMark | null>(null);
  const [snapError, setSnapError] = useState<unknown>(null);
  const [traceLevel, setTraceLevel] = useState<"visual" | "full" | "summary" | "none">("visual");
  const [reps, setReps] = useState(5);
  const geo = useDatasetGeometry(datasetId);
  const run = useRun();

  useEffect(() => {
    Promise.all([api.datasets(), api.queues(), api.models()])
      .then(([d, q, m]) => {
        setDatasets(d);
        setQueues(q);
        setModels(m);
        setDatasetId((cur) => cur ?? d.find((x) => x.id === "osm_monaco" && x.status === "READY")?.id ?? d.find((x) => x.status === "READY")?.id ?? null);
      })
      .catch(setMetaError);
  }, []);

  // A run link (/explore?run=...) reopens a stored run.
  useEffect(() => {
    const id = params.get("run");
    if (id) void run.load(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Reopened run: show its query on the map.
  useEffect(() => {
    const r = run.result;
    if (!r || !geo.coords || source) return;
    const at = (n: number): PointMark => ({ node: n, lon: geo.coords![2 * n], lat: geo.coords![2 * n + 1] });
    setSource(at(r.request.source));
    setTarget(at(r.request.target));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [run.result, geo.coords]);

  const pickDataset = (id: string) => {
    setDatasetId(id);
    setSource(null);
    setTarget(null);
    setConfig(DEFAULT_CONFIG);
  };

  const onMapClick = async (lon: number, lat: number) => {
    if (!datasetId) return;
    setSnapError(null);
    try {
      const s = await api.snap(datasetId, lon, lat);
      const mark: PointMark = { lon: s.lon, lat: s.lat, node: s.node, clicked: s.clicked, snapM: s.snap_distance_m };
      if (!source || (source && target)) {
        setSource(mark);
        setTarget(null);
      } else setTarget(mark);
    } catch (e) {
      setSnapError(e);
    }
  };

  const setNode = (which: "s" | "t", v: string) => {
    const n = Number(v);
    if (!Number.isInteger(n) || n < 0) return;
    const mark: PointMark = { node: n, lon: geo.coords ? geo.coords[2 * n] : 0, lat: geo.coords ? geo.coords[2 * n + 1] : 0 };
    if (which === "s") setSource(mark);
    else setTarget(mark);
  };

  const canRun =
    datasetId && geo.dataset?.status === "READY" && source && target &&
    (config.queue !== "lpq_model" || config.model_id) &&
    run.phase !== "submitting" && run.phase !== "running" && run.phase !== "streaming";

  const submit = async () => {
    if (!datasetId || !source || !target) return;
    const id = await run.submit({
      ...normaliseConfig(config),
      dataset_id: datasetId,
      source_node: source.node,
      target_node: target.node,
      trace_level: traceLevel,
      repetitions: reps,
      warmup: reps > 0 ? 1 : 0,
    });
    if (id) router.replace(`/explore?dataset=${datasetId}&run=${id}`, { scroll: false });
  };

  const compareHref =
    source && target && datasetId
      ? `/compare?dataset=${datasetId}&source=${source.node}&target=${target.node}&a=${encodeURIComponent(JSON.stringify(normaliseConfig(config)))}`
      : null;

  const sidebar = (
    <div className="stack">
      <h1 style={{ fontSize: 16 }}>Route Explorer</h1>
      {metaError ? <ErrorBox error={metaError} /> : null}
      {!datasets && !metaError ? <Loading label="Loading datasets…" /> : null}
      {datasets ? (
        <label className="field" htmlFor="ds">
          Dataset
          <select id="ds" value={datasetId ?? ""} onChange={(e) => pickDataset(e.target.value)} data-testid="dataset-select">
            {datasets.map((d) => (
              <option key={d.id} value={d.id} disabled={d.status !== "READY"}>
                {d.id} · {d.family} · {d.nodes ? fmtInt(d.nodes) : "?"} nodes{d.status !== "READY" ? ` (${d.status})` : ""}
              </option>
            ))}
          </select>
        </label>
      ) : null}
      {geo.error ? <ErrorBox error={geo.error} /> : null}
      {geo.loading ? <Loading label="Loading graph geometry…" lines={1} /> : null}
      {geo.dataset ? (
        <div className="small muted">
          <StateBadge state={geo.dataset.status} /> {geo.dataset.scale_mode} mode
          {geo.dataset.osm ? ` · OSM ${geo.dataset.osm.region} ${geo.dataset.osm.snapshot_date} · weights in ${geo.dataset.osm.weight_unit}s (shortest, not fastest)` : ""}
        </div>
      ) : null}

      <h3>Query</h3>
      {geo.coords ? (
        <p className="small muted">Click the map: first the source, then the destination. Clicks snap to the nearest node of the processed road graph.</p>
      ) : geo.dataset ? (
        <button type="button" onClick={async () => {
          const pr = await api.randomPair(geo.dataset!.id, Math.floor(Math.random() * 1e6));
          setNode("s", String(pr.source_node));
          setNode("t", String(pr.target_node));
        }}>Random pair (seeded)</button>
      ) : null}
      <div className="grid cols-2">
        <label className="field" htmlFor="src">
          Source node
          <input id="src" type="number" min={0} value={source?.node ?? ""} onChange={(e) => setNode("s", e.target.value)} data-testid="source-node" />
        </label>
        <label className="field" htmlFor="dst">
          Destination node
          <input id="dst" type="number" min={0} value={target?.node ?? ""} onChange={(e) => setNode("t", e.target.value)} data-testid="target-node" />
        </label>
      </div>
      {[source, target].map((m, i) =>
        m?.snapM !== undefined ? (
          <p key={i} className="small dim" style={{ margin: 0 }}>
            {i === 0 ? "Source" : "Destination"}: click snapped {fmtNum(m.snapM, 1)} m to graph node {m.node}
          </p>
        ) : null,
      )}
      {snapError ? <ErrorBox error={snapError} title="Could not snap" /> : null}

      <h3>Configuration</h3>
      <RunConfigForm value={config} onChange={setConfig} dataset={geo.dataset} queues={queues} models={models} idPrefix="ex" />
      <div className="grid cols-2">
        <label className="field" htmlFor="tl">
          Trace
          <select id="tl" value={traceLevel} onChange={(e) => setTraceLevel(e.target.value as typeof traceLevel)}>
            <option value="visual">visual</option>
            <option value="full">full (every pop/push)</option>
            <option value="summary">summary</option>
            <option value="none">none (metrics only)</option>
          </select>
        </label>
        <label className="field" htmlFor="reps">
          Timed repetitions
          <input id="reps" type="number" min={0} max={50} value={reps} onChange={(e) => setReps(Math.max(0, Math.min(50, Number(e.target.value) || 0)))} />
        </label>
      </div>
      <div className="row">
        <button type="button" className="primary" disabled={!canRun} onClick={submit} data-testid="run-button">
          Run
        </button>
        {run.phase === "running" || run.phase === "streaming" ? (
          <button type="button" className="danger" onClick={run.cancel}>Cancel</button>
        ) : null}
        {compareHref ? <Link href={compareHref} className="btn">Compare…</Link> : null}
      </div>
      {run.job ? (
        <p className="small muted">
          Run <Link href={`/runs/${run.job.id}`}>{run.job.id}</Link> {run.state ? <StateBadge state={run.state} /> : null}
        </p>
      ) : null}
    </div>
  );

  const status =
    run.phase === "submitting" || run.phase === "running" || run.phase === "streaming" ? (
      <div className="notice info small">Engine {run.state?.toLowerCase() ?? "starting"}… {run.phase === "streaming" ? "receiving trace" : ""}</div>
    ) : null;

  return (
    <RunViewer
      dataset={geo.dataset}
      coords={geo.coords}
      bbox={geo.bbox}
      events={run.events}
      result={run.result}
      source={source}
      target={target}
      onMapClick={geo.coords ? onMapClick : undefined}
      sidebar={sidebar}
      status={status}
      error={run.error}
    />
  );
}

export default function ExplorePage() {
  return (
    <Suspense fallback={<div className="page"><Loading /></div>}>
      <ExploreInner />
    </Suspense>
  );
}
