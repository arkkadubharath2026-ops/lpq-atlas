"use client";
import dynamic from "next/dynamic";
import { useMemo } from "react";
import type { PointMark } from "./MapView";
import { EventLog, LpqLive, PlayerControls, StepExplanation, Telemetry, TraceStatus } from "./RunPanels";
import { Empty, ErrorBox, Loading } from "./States";
import { useTracePlayer } from "@/lib/hooks";
import type { TraceEvent } from "@/lib/trace";
import type { Dataset, RunResult } from "@/lib/types";
import { fmtInt, isLearned } from "@/lib/format";

const MapView = dynamic(() => import("./MapView"), {
  ssr: false,
  loading: () => <div className="mapwrap" />,
});

interface Props {
  dataset: Dataset | null;
  coords: Float32Array | null;
  bbox: [number, number, number, number] | null;
  events: TraceEvent[] | null;
  result: RunResult | null;
  source: PointMark | null;
  target: PointMark | null;
  onMapClick?: (lon: number, lat: number) => void;
  status?: React.ReactNode;
  sidebar: React.ReactNode;
  error?: unknown;
}

export function RunViewer(p: Props) {
  const nodeCount = p.dataset?.nodes ?? 0;
  const player = useTracePlayer(p.events, nodeCount);
  const learned = p.result ? isLearned(p.result.request.queue) : false;
  const settledColor = learned ? "#a78bfa" : "#60a5fa";
  const r = player.replay;
  const showPath = Boolean(p.result?.result.reachable && (!r || r.done || r.completed));
  const path = useMemo(() => (showPath && p.result ? p.result.result.path : null), [showPath, p.result]);
  const mappable = Boolean(p.coords);
  const unit = p.dataset?.osm?.weight_unit;
  const algorithm = p.result ? (p.result.request.algorithm === "astar" ? "astar" : "dijkstra") : "dijkstra";

  return (
    <div className="explorer">
      <aside aria-label="Run configuration">{p.sidebar}</aside>
      <main>
        {mappable ? (
          <MapView
            datasetId={p.dataset?.id ?? null}
            coords={p.coords}
            bbox={p.bbox}
            replay={r}
            replayVersion={player.version}
            settledColor={settledColor}
            path={path}
            source={p.source}
            target={p.target}
            onMapClick={p.onMapClick}
            label="Road graph map: click to choose source, then destination"
          />
        ) : (
          <div className="mapwrap" style={{ display: "grid", placeItems: "center", padding: 24 }}>
            <div style={{ maxWidth: 520 }}>
              {p.dataset ? (
                <Empty title={`Research mode: ${p.dataset.id} has ${p.dataset.has_coordinates ? "too many nodes to draw" : "no coordinates"}`}>
                  Choose node ids in the panel. The trace is still the engine&apos;s real event stream; progress
                  is shown as counts instead of a map.
                  {r ? (
                    <div style={{ marginTop: 10 }} className="mono">
                      settled {fmtInt(r.settledCount)} · relaxed {fmtInt(r.relaxedCount)} (trace-derived)
                    </div>
                  ) : null}
                </Empty>
              ) : (
                <Empty title="Choose a dataset to begin" />
              )}
            </div>
          </div>
        )}
        <div className="legend" style={{ padding: "6px 12px", background: "var(--surface)", borderTop: "1px solid var(--border)" }}>
          <span><i style={{ background: settledColor }} />settled ({learned ? "learned queue" : "classical"})</span>
          <span><i style={{ background: "#7dd3fc" }} />frontier (relaxed, not settled)</span>
          <span><i style={{ background: "#2dd4bf" }} />returned route</span>
          <span><i style={{ background: "#2dd4bf", borderRadius: 9 }} />source</span>
          <span><i style={{ background: "#f87171", borderRadius: 9 }} />destination</span>
          <span><i style={{ background: "#3b5068" }} />processed road graph</span>
          {r ? <span className="mono">trace: settled {fmtInt(r.settledCount)}</span> : null}
        </div>
        <PlayerControls
          playing={player.playing}
          setPlaying={player.setPlaying}
          speed={player.speed}
          setSpeed={player.setSpeed}
          index={r?.index ?? 0}
          total={p.events?.length ?? 0}
          onStep={() => player.step(1)}
          onStepSettle={player.stepSettle}
          onRestart={player.restart}
          onSeek={player.seek}
          onFinish={player.finish}
          disabled={!p.events || p.events.length === 0}
        />
      </main>
      <section className="side" aria-label="Telemetry">
        <div className="stack">
          {p.status}
          {p.error ? <ErrorBox error={p.error} /> : null}
          {p.result ? (
            <>
              <TraceStatus r={p.result} events={p.events} />
              <StepExplanation event={player.current} algorithm={algorithm} />
              {r && p.result.lpq ? <LpqLive replay={r} /> : null}
              {p.events && p.events.length ? <EventLog events={p.events} index={r?.index ?? 0} /> : null}
              <Telemetry r={p.result} unit={unit} />
            </>
          ) : !p.error && !p.status ? (
            <Empty title="No run yet">Configure a query and press Run. Results appear here.</Empty>
          ) : null}
          {!p.result && p.status && !p.error ? <Loading label="Waiting for the engine…" lines={4} /> : null}
        </div>
      </section>
    </div>
  );
}
