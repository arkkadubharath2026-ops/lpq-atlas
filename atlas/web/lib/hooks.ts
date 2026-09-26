"use client";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { api, type RunBody } from "./api";
import { Replay, type TraceEvent } from "./trace";
import type { Dataset, Job, RunResult } from "./types";

/** Dataset metadata plus, for mappable datasets, its node coordinates. */
export function useDatasetGeometry(datasetId: string | null) {
  const [dataset, setDataset] = useState<Dataset | null>(null);
  const [coords, setCoords] = useState<Float32Array | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    let live = true;
    setDataset(null);
    setCoords(null);
    setError(null);
    if (!datasetId) return;
    setLoading(true);
    (async () => {
      try {
        const d = await api.dataset(datasetId);
        if (!live) return;
        setDataset(d);
        if (d.status === "READY" && d.coordinate_system === "geographic" && d.scale_mode !== "research") {
          const c = await api.coords(datasetId);
          if (live) setCoords(c);
        }
      } catch (e) {
        if (live) setError(e);
      } finally {
        if (live) setLoading(false);
      }
    })();
    return () => {
      live = false;
    };
  }, [datasetId]);

  const bbox: [number, number, number, number] | null =
    dataset?.osm?.bbox ?? (coords ? bboxOf(coords) : null);
  return { dataset, coords, bbox, error, loading };
}

function bboxOf(c: Float32Array): [number, number, number, number] {
  let w = Infinity,
    s = Infinity,
    e = -Infinity,
    n = -Infinity;
  for (let i = 0; i < c.length; i += 2) {
    if (c[i] < w) w = c[i];
    if (c[i] > e) e = c[i];
    if (c[i + 1] < s) s = c[i + 1];
    if (c[i + 1] > n) n = c[i + 1];
  }
  return [w, s, e, n];
}

/** Drives a Replay: play/pause/step/seek at a speed in events per second. */
export function useTracePlayer(events: TraceEvent[] | null, nodeCount: number) {
  // The Replay is a mutable, non-React object (typed arrays for up to ~10^6
  // nodes); a version counter tells React when it has advanced.
  const replay = useMemo(
    () => (events && nodeCount > 0 ? new Replay(events, nodeCount) : null),
    [events, nodeCount],
  );
  const [version, setVersion] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(2000);
  const [current, setCurrent] = useState<TraceEvent | null>(null);

  useEffect(() => {
    setCurrent(null);
    setPlaying(Boolean(replay && replay.events.length));
  }, [replay]);

  useEffect(() => {
    if (!playing || !replay) return;
    let raf = 0;
    let lastT = 0;
    let budget = 0;
    const tick = (t: number) => {
      const dt = lastT ? Math.min(100, t - lastT) : 16;
      lastT = t;
      budget += (speed * dt) / 1000;
      let last: TraceEvent | null = null;
      while (budget >= 1 && !replay.done) {
        last = replay.step();
        budget -= 1;
      }
      if (last) setCurrent(last);
      setVersion((v) => v + 1);
      if (replay.done) {
        setPlaying(false);
        return;
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [playing, speed, replay]);

  const step = (n = 1) => {
    if (!replay) return;
    setPlaying(false);
    let last: TraceEvent | null = null;
    for (let i = 0; i < n && !replay.done; i++) last = replay.step();
    if (last) setCurrent(last);
    setVersion((v) => v + 1);
  };
  const stepSettle = () => {
    if (!replay) return;
    setPlaying(false);
    const last = replay.advanceSettled(1);
    if (last) setCurrent(last);
    setVersion((v) => v + 1);
  };
  const restart = () => {
    replay?.reset();
    setCurrent(null);
    setVersion((v) => v + 1);
  };
  const seek = (i: number) => {
    if (!replay) return;
    replay.seek(i);
    setCurrent(replay.index > 0 ? replay.events[replay.index - 1] : null);
    setVersion((v) => v + 1);
  };
  const finish = () => seek(events?.length ?? 0);

  return { replay, version, playing, setPlaying, speed, setSpeed, current, step, stepSettle, restart, seek, finish };
}

export type RunPhase = "idle" | "submitting" | "running" | "streaming" | "done" | "failed";

/** Submits a run, streams its trace over SSE, then loads the authoritative result. */
export function useRun() {
  const [phase, setPhase] = useState<RunPhase>("idle");
  const [job, setJob] = useState<Job | null>(null);
  const [state, setState] = useState<string | null>(null);
  const [events, setEvents] = useState<TraceEvent[] | null>(null);
  const [result, setResult] = useState<RunResult | null>(null);
  const [error, setError] = useState<unknown>(null);
  const close = useRef<(() => void) | null>(null);

  useEffect(() => () => close.current?.(), []);

  const load = useCallback(async (id: string) => {
    close.current?.();
    setError(null);
    setEvents(null);
    setResult(null);
    setPhase("running");
    const buf: TraceEvent[] = [];
    close.current = api.streamEvents(id, {
      onState: (s) => setState(s),
      onBatch: (b) => {
        setPhase("streaming");
        for (const e of b) buf.push(e);
      },
      onEnd: async (info) => {
        setState(info.state);
        try {
          const j = await api.run(id);
          setJob(j);
          if (info.state !== "COMPLETED") {
            setPhase("failed");
            setError(new Error(j.error?.message ?? `Run ${info.state.toLowerCase()}`));
            return;
          }
          setResult(await api.runResult(id));
          setEvents(info.trace ? buf : []);
          setPhase("done");
        } catch (e) {
          setError(e);
          setPhase("failed");
        }
      },
      onError: (m) => {
        setError(new Error(m));
        setPhase("failed");
      },
    });
  }, []);

  const submit = useCallback(
    async (body: RunBody) => {
      setPhase("submitting");
      setError(null);
      setEvents(null);
      setResult(null);
      try {
        const j = await api.createRun(body);
        setJob(j);
        setState(j.state);
        await load(j.id);
        return j.id;
      } catch (e) {
        setError(e);
        setPhase("failed");
        return null;
      }
    },
    [load],
  );

  const cancel = useCallback(async () => {
    if (job) await api.cancelRun(job.id).catch(() => undefined);
  }, [job]);

  return { phase, job, state, events, result, error, submit, load, cancel };
}
