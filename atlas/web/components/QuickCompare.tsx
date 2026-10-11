"use client";
// Quick Compare: pick a graph, a start and a destination, press one button, and
// see vanilla and optimized Dijkstra and A* (plus optional strong non-ML queues)
// timed fairly on the real engine. ML configuration, repetition counts and queue
// internals live behind the Advanced sections.
import dynamic from "next/dynamic";
import Link from "next/link";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useEffect, useMemo, useState } from "react";
import { LaneBoard } from "@/components/LaneBoard";
import type { PointMark } from "@/components/MapView";
import { Empty, ErrorBox, Loading } from "@/components/States";
import { Tip } from "@/components/Tip";
import { api } from "@/lib/api";
import { useDatasetGeometry, useLaneComparison } from "@/lib/hooks";
import {
  ALGORITHM_TIP,
  DEFAULT_REPETITIONS,
  DEFAULT_WARMUP,
  LANES,
  MIN_REPETITIONS,
  MIN_WARMUP,
  defaultLaneIds,
  lanesForRequest,
  unavailableLaneIds,
} from "@/lib/lanes";
import { fmtInt, fmtNum } from "@/lib/format";
import type { Dataset, QueueInfo } from "@/lib/types";

const MapView = dynamic(() => import("@/components/MapView"), {
  ssr: false,
  loading: () => <div className="mapwrap" />,
});

function datasetLabel(d: Dataset): string {
  const size = d.nodes ? `${fmtInt(d.nodes)} places` : "size unknown";
  return `${d.id} · ${d.family} · ${size}${d.status !== "READY" ? ` (${d.status.replace(/_/g, " ").toLowerCase()})` : ""}`;
}

type Picked = Omit<PointMark, "lon" | "lat"> & { lon?: number; lat?: number };

function nodeParam(v: string | null): Picked | null {
  if (v === null || v === "") return null;
  const n = Number(v);
  return Number.isInteger(n) && n >= 0 ? { node: n } : null;
}

function pickDefaultDataset(list: Dataset[], wanted: string | null): string | null {
  const ready = list.filter((d) => d.status === "READY");
  if (wanted && ready.some((d) => d.id === wanted)) return wanted;
  return (
    ready.find((d) => d.id === "osm_monaco")?.id ??
    ready.find((d) => d.has_coordinates && d.scale_mode !== "research")?.id ??
    ready[0]?.id ??
    null
  );
}

function QuickCompareInner() {
  const params = useSearchParams();
  const router = useRouter();
  const [datasets, setDatasets] = useState<Dataset[] | null>(null);
  const [queues, setQueues] = useState<QueueInfo[]>([]);
  const [metaError, setMetaError] = useState<unknown>(null);
  const [datasetId, setDatasetId] = useState<string | null>(null);
  // A picked place is a node id plus, when it came from a map click, the snap details.
  // Its map position is derived from the coordinates once they are loaded, so a
  // deep link (?source=&target=) needs no effect to place the markers.
  const [pickedSource, setSource] = useState<Picked | null>(() => nodeParam(params.get("source")));
  const [pickedTarget, setTarget] = useState<Picked | null>(() => nodeParam(params.get("target")));
  const [snapError, setSnapError] = useState<unknown>(null);
  const [lanesOn, setLanesOn] = useState<Set<string>>(() => new Set(defaultLaneIds()));
  const [reps, setReps] = useState(DEFAULT_REPETITIONS);
  const [warmup, setWarmup] = useState(DEFAULT_WARMUP);
  const [heuristic, setHeuristic] = useState<"auto" | "zero" | "haversine" | "euclidean">("auto");
  const [seedText, setSeedText] = useState("");
  const geo = useDatasetGeometry(datasetId);
  const cmp = useLaneComparison();
  const place = (p: Picked | null): PointMark | null =>
    p
      ? {
          ...p,
          lon: p.lon ?? (geo.coords ? geo.coords[2 * p.node] : 0),
          lat: p.lat ?? (geo.coords ? geo.coords[2 * p.node + 1] : 0),
        }
      : null;
  const source = place(pickedSource);
  const target = place(pickedTarget);

  useEffect(() => {
    Promise.all([api.datasets(), api.queues()])
      .then(([d, q]) => {
        setDatasets(d);
        setQueues(q);
        setDatasetId(pickDefaultDataset(d, params.get("dataset")));
      })
      .catch(setMetaError);
    // The URL only seeds the first selection.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const unavailable = useMemo(() => unavailableLaneIds(queues), [queues]);
  const busy = cmp.phase === "submitting" || cmp.phase === "running";
  const selectedCount = lanesOn.size;
  const canRun = Boolean(
    datasetId && geo.dataset?.status === "READY" && source && target && selectedCount >= 2 && !busy,
  );

  const pickDataset = (id: string) => {
    setDatasetId(id);
    setSource(null);
    setTarget(null);
    setHeuristic("auto");
  };

  const onMapClick = async (lon: number, lat: number) => {
    if (!datasetId || busy) return;
    setSnapError(null);
    try {
      const s = await api.snap(datasetId, lon, lat);
      const mark: PointMark = { lon: s.lon, lat: s.lat, node: s.node, clicked: s.clicked, snapM: s.snap_distance_m };
      if (!pickedSource || pickedTarget) {
        setSource(mark);
        setTarget(null);
      } else setTarget(mark);
    } catch (e) {
      setSnapError(e);
    }
  };

  const setNode = (which: "s" | "t", v: string) => {
    const n = Number(v);
    const next = v !== "" && Number.isInteger(n) && n >= 0 ? { node: n } : null;
    if (v !== "" && next === null) return;
    if (which === "s") setSource(next);
    else setTarget(next);
  };

  const randomPair = async () => {
    if (!datasetId) return;
    setSnapError(null);
    try {
      const p = await api.randomPair(datasetId, Math.floor(Math.random() * 1e6));
      setSource({ node: p.source_node });
      setTarget({ node: p.target_node });
    } catch (e) {
      setSnapError(e);
    }
  };

  const toggleLane = (id: string, on: boolean) =>
    setLanesOn((cur) => {
      const next = new Set(cur);
      if (on) next.add(id);
      else next.delete(id);
      return next;
    });

  const run = async () => {
    if (!datasetId || !source || !target) return;
    const seed = seedText.trim() === "" ? undefined : Number(seedText);
    await cmp.submit({
      dataset_id: datasetId,
      source_node: source.node,
      target_node: target.node,
      lanes: lanesForRequest(lanesOn),
      heuristic,
      repetitions: reps,
      warmup,
      ...(seed !== undefined && Number.isInteger(seed) && seed >= 0 ? { order_seed: seed } : {}),
    });
    router.replace(`/?dataset=${datasetId}&source=${source.node}&target=${target.node}`, { scroll: false });
  };

  const optionalLanes = LANES.filter((l) => l.optional);
  const coreLanes = LANES.filter((l) => !l.optional);
  const missingOptimized = coreLanes.some((l) => l.kind === "optimized" && unavailable.has(l.id) && lanesOn.has(l.id));
  const heuristics = geo.dataset?.astar_heuristics ?? ["auto", "zero"];
  const mappable = Boolean(geo.coords);

  return (
    <div className="page stack">
      <div className="qc-hero">
        <h1>Which shortest-path method is fastest on your route?</h1>
        <p>
          Pick a map, choose a start and a destination, and press <strong>Run Comparison</strong>. Atlas runs each method on the real
          engine, one after another, and shows who finishes first and whether the answer is the same.
        </p>
      </div>

      {metaError ? <ErrorBox error={metaError} /> : null}
      {!datasets && !metaError ? <Loading label="Loading graphs…" /> : null}

      {datasets ? (
        <div className="qc-layout">
          <div className="stack">
            <section className="panel qc-step" aria-labelledby="s1">
              <h2 id="s1"><span className="n">1</span>Pick a graph</h2>
              <label className="field" htmlFor="qc-ds">
                Road or social network to search
                <select id="qc-ds" value={datasetId ?? ""} onChange={(e) => pickDataset(e.target.value)} data-testid="dataset-select" disabled={busy}>
                  {datasets.map((d) => (
                    <option key={d.id} value={d.id} disabled={d.status !== "READY"}>{datasetLabel(d)}</option>
                  ))}
                </select>
              </label>
              {geo.loading ? <Loading label="Loading the map…" lines={1} /> : null}
              {geo.error ? <ErrorBox error={geo.error} /> : null}
              {geo.dataset?.osm ? (
                <p className="small muted" style={{ margin: 0 }}>
                  OpenStreetMap {geo.dataset.osm.region}. Routes are the shortest, not the fastest, by road length.
                </p>
              ) : null}
            </section>

            <section className="panel qc-step" aria-labelledby="s2">
              <h2 id="s2"><span className="n">2</span>Choose start and destination</h2>
              <p className="small muted" style={{ margin: 0 }}>
                {mappable
                  ? "Click the map: first the start, then the destination. Clicks snap to the nearest place on the graph."
                  : "This graph is too large or has no coordinates to draw. Use random places or type node numbers."}
              </p>
              <div className="grid cols-2">
                <label className="field" htmlFor="qc-src">
                  Start (node)
                  <input id="qc-src" type="number" min={0} value={source?.node ?? ""} onChange={(e) => setNode("s", e.target.value)} data-testid="source-node" disabled={busy} />
                </label>
                <label className="field" htmlFor="qc-dst">
                  Destination (node)
                  <input id="qc-dst" type="number" min={0} value={target?.node ?? ""} onChange={(e) => setNode("t", e.target.value)} data-testid="target-node" disabled={busy} />
                </label>
              </div>
              <div className="row">
                <button type="button" onClick={randomPair} disabled={!geo.dataset || busy} data-testid="random-pair">Pick a random route</button>
                <button type="button" onClick={() => { setSource(null); setTarget(null); }} disabled={busy || (!pickedSource && !pickedTarget)}>Clear</button>
              </div>
              {[source, target].map((m, i) =>
                m?.snapM !== undefined ? (
                  <p key={i} className="small dim" style={{ margin: 0 }}>
                    {i === 0 ? "Start" : "Destination"}: your click snapped {fmtNum(m.snapM, 1)} m to graph node {m.node}.
                  </p>
                ) : null,
              )}
              {snapError ? <ErrorBox error={snapError} title="Could not pick that place" /> : null}
            </section>

            <section className="panel qc-step" aria-labelledby="s3">
              <h2 id="s3"><span className="n">3</span>Compare</h2>
              <p className="small muted" style={{ margin: 0 }}>
                Always compared: Vanilla and Optimized versions of both algorithms.
                <Tip text={ALGORITHM_TIP.dijkstra + " " + ALGORITHM_TIP.astar} />
              </p>
              <div className="stack" style={{ gap: 4 }}>
                {optionalLanes.map((l) => (
                  <label key={l.id} className="check">
                    <input type="checkbox" checked={lanesOn.has(l.id)} disabled={busy} onChange={(e) => toggleLane(l.id, e.target.checked)} data-testid={`lane-toggle-${l.id}`} />
                    Also race {l.label}
                    <Tip text={l.tooltip} />
                  </label>
                ))}
              </div>
              {missingOptimized ? (
                <div className="notice warn small" role="status" data-testid="optimized-missing">
                  This engine build does not provide the Optimized queue yet, so the Optimized lanes will show an error. The other lanes will still run.
                </div>
              ) : null}
              <button type="button" className="run-big" onClick={run} disabled={!canRun} data-testid="run-comparison">
                {busy ? "Running…" : "Run Comparison"}
              </button>
              {!canRun && !busy ? (
                <p className="small dim" style={{ margin: 0 }}>
                  {!source || !target ? "Choose a start and a destination first." : selectedCount < 2 ? "Select at least two methods." : ""}
                </p>
              ) : null}
              {busy ? (
                <div className="row">
                  <span className="small muted" role="status" data-testid="progress">{cmp.job?.progress ?? "Starting the engine…"}</span>
                  <button type="button" className="danger" onClick={cmp.cancel}>Cancel</button>
                </div>
              ) : null}
            </section>

            <details className="advanced" data-testid="setup-advanced">
              <summary>Advanced settings</summary>
              <div className="adv-body stack">
                <div className="grid cols-2">
                  <label className="field" htmlFor="qc-reps">
                    Timed repetitions per method
                    <input id="qc-reps" type="number" min={MIN_REPETITIONS} max={51} value={reps} disabled={busy}
                      onChange={(e) => setReps(Math.max(MIN_REPETITIONS, Math.min(51, Number(e.target.value) || DEFAULT_REPETITIONS)))} data-testid="repetitions" />
                  </label>
                  <label className="field" htmlFor="qc-warm">
                    Warm-up runs (discarded)
                    <input id="qc-warm" type="number" min={MIN_WARMUP} max={10} value={warmup} disabled={busy}
                      onChange={(e) => setWarmup(Math.max(MIN_WARMUP, Math.min(10, Number(e.target.value) || DEFAULT_WARMUP)))} data-testid="warmup" />
                  </label>
                </div>
                <p className="small muted" style={{ margin: 0 }}>
                  Every method gets the same counts. More repetitions give a steadier median but take longer.
                </p>
                <label className="field" htmlFor="qc-h">
                  Distance estimate shared by both A* lanes
                  <select id="qc-h" value={heuristic} onChange={(e) => setHeuristic(e.target.value as typeof heuristic)} disabled={busy}>
                    {heuristics.map((h) => (
                      <option key={h} value={h}>{h === "auto" ? "automatic (from the graph's coordinates, else none)" : h}</option>
                    ))}
                  </select>
                </label>
                <label className="field" htmlFor="qc-seed">
                  Order seed (blank = random)
                  <input id="qc-seed" type="text" inputMode="numeric" value={seedText} onChange={(e) => setSeedText(e.target.value.replace(/\D/g, ""))} placeholder="random" disabled={busy} />
                </label>
                <div className="stack" style={{ gap: 4 }}>
                  <h3>Methods</h3>
                  {coreLanes.map((l) => (
                    <label key={l.id} className="check">
                      <input type="checkbox" checked={lanesOn.has(l.id)} disabled={busy} onChange={(e) => toggleLane(l.id, e.target.checked)} />
                      {l.label}
                      {unavailable.has(l.id) ? <span className="badge warn" style={{ marginLeft: 6 }}>not in this engine build</span> : null}
                    </label>
                  ))}
                  <p className="small dim" style={{ margin: 0 }}>
                    A method is compared against the vanilla (binary heap) method that uses the same algorithm, so keep both of a pair.
                  </p>
                </div>
                <p className="small muted" style={{ margin: 0 }}>
                  Want the step-by-step search animation, ML model settings or approximate mode? Use the{" "}
                  <Link href="/explore">Route Explorer</Link> or <Link href="/compare">two-way Compare</Link>.
                </p>
              </div>
            </details>
          </div>

          <div className="stack">
            {mappable ? (
              <div className="qc-map" data-testid="qc-map">
                <MapView
                  datasetId={datasetId}
                  coords={geo.coords}
                  bbox={geo.bbox}
                  replay={null}
                  replayVersion={0}
                  settledColor="#60a5fa"
                  path={cmp.result && cmp.result.path.length ? cmp.result.path : null}
                  source={source}
                  target={target}
                  onMapClick={busy ? undefined : onMapClick}
                  label="Road graph map: click to choose the start, then the destination"
                />
              </div>
            ) : geo.dataset ? (
              <Empty title={`No map for ${geo.dataset.id}`}>
                {geo.dataset.has_coordinates ? "It has too many places to draw quickly." : "It has no coordinates."} Use a random route or type node numbers; the comparison works the same.
              </Empty>
            ) : null}

            {cmp.phase === "idle" ? (
              <Empty title="No comparison yet">Choose a start and a destination, then press Run Comparison.</Empty>
            ) : null}
            {busy ? <Loading label={cmp.job?.progress ?? "Timing each method…"} lines={4} /> : null}
            {cmp.error ? <ErrorBox error={cmp.error} title="The comparison could not finish" /> : null}
            {cmp.result ? <LaneBoard result={cmp.result} dataset={geo.dataset} /> : null}
            {cmp.result && datasetId ? (
              <p className="small muted">
                Want to watch the search step by step?{" "}
                <Link href={`/compare?dataset=${datasetId}&source=${cmp.result.source_node}&target=${cmp.result.target_node}`}>Open this query in the detailed Compare view</Link>.
              </p>
            ) : null}
          </div>
        </div>
      ) : null}
    </div>
  );
}

export function QuickCompare() {
  return (
    <Suspense fallback={<div className="page"><Loading /></div>}>
      <QuickCompareInner />
    </Suspense>
  );
}
