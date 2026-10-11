"use client";
// Compare mode (spec section 13): the same graph and the same query under two
// configurations, replayed side by side and synchronised by algorithm progress
// (settled nodes), never by wall clock. Runtimes shown are the clean,
// untraced benchmark medians from each run's result.
import dynamic from "next/dynamic";
import Link from "next/link";
import { Suspense, useEffect, useMemo, useRef, useState } from "react";
import { useSearchParams } from "next/navigation";
import { RunConfigForm, DEFAULT_CONFIG, normaliseConfig } from "@/components/RunConfigForm";
import type { PointMark } from "@/components/MapView";
import { LpqLive, TraceStatus } from "@/components/RunPanels";
import { Empty, ErrorBox, Loading, StateBadge } from "@/components/States";
import { api, waitForJob } from "@/lib/api";
import { useDatasetGeometry } from "@/lib/hooks";
import { Replay, type TraceEvent } from "@/lib/trace";
import type { Dataset, EngineConfig, Job, ModelInfo, QueueInfo, RunResult } from "@/lib/types";
import { fmtDistance, fmtInt, fmtNs, fmtNum, fmtPct, isLearned, queueLabel } from "@/lib/format";

const MapView = dynamic(() => import("@/components/MapView"), { ssr: false, loading: () => <div className="mapwrap" /> });

interface Side {
  runId: string;
  result: RunResult;
  events: TraceEvent[];
}

function parseConfig(s: string | null): EngineConfig | null {
  if (!s) return null;
  try {
    return normaliseConfig({ ...DEFAULT_CONFIG, ...(JSON.parse(s) as Partial<EngineConfig>) });
  } catch {
    return null;
  }
}

function CompareInner() {
  const params = useSearchParams();
  const [datasets, setDatasets] = useState<Dataset[] | null>(null);
  const [queues, setQueues] = useState<QueueInfo[]>([]);
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [datasetId, setDatasetId] = useState<string | null>(params.get("dataset"));
  const [a, setA] = useState<EngineConfig>(parseConfig(params.get("a")) ?? { ...DEFAULT_CONFIG, queue: "radix_heap" });
  const [b, setB] = useState<EngineConfig>(parseConfig(params.get("b")) ?? { ...DEFAULT_CONFIG, queue: "lpq" });
  const [source, setSource] = useState<PointMark | null>(null);
  const [target, setTarget] = useState<PointMark | null>(null);
  const [job, setJob] = useState<Job | null>(null);
  const [sides, setSides] = useState<[Side, Side] | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const geo = useDatasetGeometry(datasetId);

  useEffect(() => {
    Promise.all([api.datasets(), api.queues(), api.models()])
      .then(([d, q, m]) => {
        setDatasets(d);
        setQueues(q);
        setModels(m);
        setDatasetId((cur) => cur ?? d.find((x) => x.id === "osm_monaco")?.id ?? d[0]?.id ?? null);
      })
      .catch(setError);
  }, []);

  useEffect(() => {
    const s = params.get("source");
    const t = params.get("target");
    if (s !== null && t !== null && geo.dataset) {
      const at = (n: number): PointMark => ({ node: n, lon: geo.coords ? geo.coords[2 * n] : 0, lat: geo.coords ? geo.coords[2 * n + 1] : 0 });
      setSource(at(Number(s)));
      setTarget(at(Number(t)));
    }
  }, [params, geo.dataset, geo.coords]);

  const onMapClick = async (lon: number, lat: number) => {
    if (!datasetId) return;
    try {
      const s = await api.snap(datasetId, lon, lat);
      const m: PointMark = { lon: s.lon, lat: s.lat, node: s.node, clicked: s.clicked, snapM: s.snap_distance_m };
      if (!source || (source && target)) {
        setSource(m);
        setTarget(null);
      } else setTarget(m);
    } catch (e) {
      setError(e);
    }
  };

  const run = async () => {
    if (!datasetId || !source || !target) return;
    setBusy(true);
    setError(null);
    setSides(null);
    try {
      const j = await api.createComparison({
        dataset_id: datasetId,
        source_node: source.node,
        target_node: target.node,
        a: { ...normaliseConfig(a), label: "A" },
        b: { ...normaliseConfig(b), label: "B" },
        trace_level: "visual",
        repetitions: 7,
        warmup: 1,
      });
      setJob(j);
      const done = await waitForJob(() => api.comparison(j.id), setJob);
      if (done.state !== "COMPLETED") throw new Error(done.error?.message ?? `Comparison ${done.state.toLowerCase()}`);
      const ids = done.summary.runs as string[];
      const loaded = await Promise.all(
        ids.map(async (id) => ({ runId: id, result: await api.runResult(id), events: await api.runTrace(id).catch(() => []) })),
      );
      setSides([loaded[0], loaded[1]]);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div>
      <div className="page stack" style={{ paddingBottom: 8 }}>
        <div className="row" style={{ justifyContent: "space-between" }}>
          <h1>Compare</h1>
          <span className="small muted">Same dataset · same query · same timing boundary. No winner is declared; measurements are shown.</span>
        </div>
        {error ? <ErrorBox error={error} /> : null}
        <div className="grid cols-4">
          <div className="panel stack">
            <h3>Query</h3>
            {!datasets ? <Loading /> : (
              <label className="field">Dataset
                <select value={datasetId ?? ""} onChange={(e) => { setDatasetId(e.target.value); setSource(null); setTarget(null); setSides(null); }}>
                  {datasets.map((d) => <option key={d.id} value={d.id} disabled={d.status !== "READY"}>{d.id}</option>)}
                </select>
              </label>
            )}
            <div className="grid cols-2">
              <label className="field">Source
                <input type="number" min={0} value={source?.node ?? ""} onChange={(e) => setSource({ node: Number(e.target.value), lon: geo.coords?.[2 * Number(e.target.value)] ?? 0, lat: geo.coords?.[2 * Number(e.target.value) + 1] ?? 0 })} />
              </label>
              <label className="field">Destination
                <input type="number" min={0} value={target?.node ?? ""} onChange={(e) => setTarget({ node: Number(e.target.value), lon: geo.coords?.[2 * Number(e.target.value)] ?? 0, lat: geo.coords?.[2 * Number(e.target.value) + 1] ?? 0 })} />
              </label>
            </div>
            <p className="small muted">{geo.coords ? "Or click the left map: source, then destination." : "This dataset has no map; enter node ids."}</p>
            <button className="primary" type="button" disabled={busy || !source || !target} onClick={run} data-testid="compare-run">
              {busy ? "Running…" : "Run both"}
            </button>
            {job ? <div className="small">comparison {job.id} <StateBadge state={job.state} /></div> : null}
          </div>
          <div className="panel"><h3>A</h3><RunConfigForm value={a} onChange={setA} dataset={geo.dataset} queues={queues} models={models} idPrefix="a" /></div>
          <div className="panel"><h3>B</h3><RunConfigForm value={b} onChange={setB} dataset={geo.dataset} queues={queues} models={models} idPrefix="b" /></div>
          <div className="panel stack">
            <h3>Reference</h3>
            {sides ? (
              <>
                <dl className="kv">
                  <dt>Exact reference</dt><dd>{fmtDistance(sides[0].result.reference.distance, geo.dataset?.osm?.weight_unit)}</dd>
                  <dt>Reference agrees</dt><dd>{sides[0].result.reference.distance === sides[1].result.reference.distance ? "yes" : "NO"}</dd>
                </dl>
                <p className="small muted">Computed independently in each run by <code>reference_dijkstra_heap</code>.</p>
              </>
            ) : <Empty title="Run a comparison" />}
          </div>
        </div>
      </div>
      {sides ? <CompareView sides={sides} dataset={geo.dataset} coords={geo.coords} bbox={geo.bbox} /> : (
        <div style={{ height: "55vh", display: "grid" }}>
          <MapView datasetId={datasetId} coords={geo.coords} bbox={geo.bbox} replay={null} replayVersion={0} settledColor="#60a5fa"
            path={null} source={source} target={target} onMapClick={geo.coords ? onMapClick : undefined} label="Choose the comparison query" />
        </div>
      )}
    </div>
  );
}

function CompareView({ sides, dataset, coords, bbox }: { sides: [Side, Side]; dataset: Dataset | null; coords: Float32Array | null; bbox: [number, number, number, number] | null }) {
  const n = dataset?.nodes ?? 0;
  const replays = useMemo(() => sides.map((s) => new Replay(s.events, n)) as [Replay, Replay], [sides, n]);
  const [version, setVersion] = useState(0);
  const [playing, setPlaying] = useState(true);
  const [perTick, setPerTick] = useState(20);
  const [sync, setSync] = useState(true);
  const raf = useRef<number | null>(null);

  useEffect(() => {
    if (!playing) return;
    const tick = () => {
      let progressed = false;
      for (const r of replays) {
        if (!r.done) {
          // Synchronised: both advance by the same number of SETTLED nodes.
          if (sync) r.advanceSettled(perTick);
          else for (let i = 0; i < perTick * 3 && !r.done; i++) r.step();
          progressed = true;
        }
      }
      setVersion((v) => v + 1);
      if (!progressed) {
        setPlaying(false);
        return;
      }
      raf.current = requestAnimationFrame(tick);
    };
    raf.current = requestAnimationFrame(tick);
    return () => { if (raf.current) cancelAnimationFrame(raf.current); };
  }, [playing, perTick, sync, replays]);

  const restart = () => { replays.forEach((r) => r.reset()); setVersion((v) => v + 1); };
  const stepOnce = () => { setPlaying(false); replays.forEach((r) => r.advanceSettled(1)); setVersion((v) => v + 1); };
  const unit = dataset?.osm?.weight_unit;
  const [ra, rb] = [sides[0].result, sides[1].result];
  const at = (n: number): PointMark | null =>
    coords ? { node: n, lon: coords[2 * n], lat: coords[2 * n + 1] } : null;
  const ends: [PointMark | null, PointMark | null] = [at(ra.request.source), at(ra.request.target)];

  return (
    <div>
      <div className="player">
        <button type="button" onClick={() => setPlaying(!playing)}>{playing ? "❚❚ Pause" : "▶ Play"}</button>
        <button type="button" onClick={stepOnce}>Step (1 settled node each)</button>
        <button type="button" onClick={restart}>Restart</button>
        <label className="check"><input type="checkbox" checked={sync} onChange={(e) => setSync(e.target.checked)} /> synchronise by settled nodes</label>
        <label className="small muted row" style={{ gap: 4 }}>speed
          <select value={perTick} onChange={(e) => setPerTick(Number(e.target.value))} style={{ width: 130 }}>
            {[1, 5, 20, 100, 500].map((v) => <option key={v} value={v}>{v} nodes / frame</option>)}
          </select>
        </label>
      </div>
      <div className="split">
        {sides.map((s, i) => (
          <div key={s.runId}>
            <div className="row" style={{ padding: "6px 10px", background: "var(--surface)", justifyContent: "space-between" }}>
              <strong>{i === 0 ? "A" : "B"}: {s.result.request.algorithm === "astar" ? "A*" : "Dijkstra"} + {queueLabel(s.result.request.queue)}{s.result.request.mode === "approximate" ? ` (ε=${s.result.request.epsilon})` : ""}</strong>
              <span className="mono small">settled {fmtInt(replays[i].settledCount)} / {fmtInt(s.result.counters?.settled_nodes)} · <Link href={`/runs/${s.runId}`}>run</Link></span>
            </div>
            {coords ? (
              <div style={{ height: 460, display: "flex" }}>
              <MapView datasetId={dataset?.id ?? null} coords={coords} bbox={bbox} replay={replays[i]} replayVersion={version}
                settledColor={isLearned(s.result.request.queue) ? "#a78bfa" : "#60a5fa"}
                path={replays[i].done ? s.result.result.path : null} source={ends[0]} target={ends[1]} showRoads={false} label={`Replay ${i === 0 ? "A" : "B"}`} />
              </div>
            ) : (
              <div className="mapwrap" style={{ display: "grid", placeItems: "center", height: 460, flex: "none" }}>
                <span className="mono">settled {fmtInt(replays[i].settledCount)} (trace-derived)</span>
              </div>
            )}
            <div style={{ padding: 10 }}><TraceStatus r={s.result} events={s.events} /></div>
            {s.result.lpq ? <div style={{ padding: 10 }}><LpqLive replay={replays[i]} /></div> : null}
          </div>
        ))}
      </div>
      <div className="page">
        <div className="panel scroll-x">
          <h2>Measured differences</h2>
          <p className="small muted">Authoritative values from each run&apos;s result. Runtime = median of untraced repetitions under the same clock boundary. B/A &lt; 1 means B used less.</p>
          <table className="data">
            <thead><tr><th>Metric</th><th className="num">A</th><th className="num">B</th><th className="num">B / A</th></tr></thead>
            <tbody>
              {rows(ra, rb, unit).map(([name, va, vb, fmt]) => (
                <tr key={name}>
                  <td>{name}</td>
                  <td className="num">{fmt(va)}</td>
                  <td className="num">{fmt(vb)}</td>
                  <td className="num">{va && vb !== null && va !== null ? fmtNum((vb as number) / (va as number), 3) : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}

type Row = [string, number | null, number | null, (v: number | null) => string];
function rows(a: RunResult, b: RunResult, unit?: string): Row[] {
  const ns = (v: number | null) => fmtNs(v);
  const int = (v: number | null) => fmtInt(v);
  const out: Row[] = [
    ["Returned distance", a.result.distance, b.result.distance, (v) => fmtDistance(v, unit)],
    ["Approximation ratio", a.correctness.approximation_ratio, b.correctness.approximation_ratio, (v) => fmtNum(v, 6)],
    ["Benchmark runtime (median)", a.timing.benchmark?.median_ns ?? null, b.timing.benchmark?.median_ns ?? null, ns],
    ["Nodes settled", a.counters?.settled_nodes ?? null, b.counters?.settled_nodes ?? null, int],
    ["Edges examined", a.counters?.edges_examined ?? null, b.counters?.edges_examined ?? null, int],
    ["Pushes", a.counters?.pq_pushes ?? null, b.counters?.pq_pushes ?? null, int],
    ["Pops", a.counters?.pq_pops ?? null, b.counters?.pq_pops ?? null, int],
    ["Comparisons (incl. verification)", a.counters?.pq_comparisons ?? null, b.counters?.pq_comparisons ?? null, int],
    ["Verification comparisons", a.lpq?.verification?.comparisons ?? null, b.lpq?.verification?.comparisons ?? null, int],
    ["Candidate entries examined", a.lpq?.metrics?.entries_examined ?? null, b.lpq?.metrics?.entries_examined ?? null, int],
    ["Fallback scans", a.lpq?.metrics?.fallback_scans ?? null, b.lpq?.metrics?.fallback_scans ?? null, int],
    ["Fallback rate", a.lpq?.metrics?.fallback_rate ?? null, b.lpq?.metrics?.fallback_rate ?? null, (v) => fmtPct(v, 2)],
    ["Score precomputation (outside timing)", a.lpq?.scorer_build_ns ?? null, b.lpq?.scorer_build_ns ?? null, ns],
  ];
  return out;
}

export default function ComparePage() {
  return (
    <Suspense fallback={<div className="page"><Loading /></div>}>
      <CompareInner />
    </Suspense>
  );
}
