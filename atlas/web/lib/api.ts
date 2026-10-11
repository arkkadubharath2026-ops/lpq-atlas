// Typed client for the orchestration API. Same-origin (/api is rewritten to the
// FastAPI server by next.config.mjs).
import type {
  Dataset,
  EngineConfig,
  Job,
  LaneComparisonJob,
  ModelInfo,
  Note,
  QueueInfo,
  ResultListing,
  ResultSummary,
  RunResult,
  Snap,
} from "./types";
import type { TraceEvent } from "./trace";

export class ApiError extends Error {
  constructor(
    public status: number,
    public body: unknown,
    message: string,
  ) {
    super(message);
  }
}

function messageOf(status: number, body: unknown): string {
  if (body && typeof body === "object") {
    const b = body as Record<string, unknown>;
    const d = (b.detail ?? b) as Record<string, unknown>;
    if (typeof d.message === "string") return d.message;
    if (Array.isArray(b.details))
      return (b.details as { field: string; message: string }[])
        .map((x) => (x.field ? `${x.field}: ${x.message}` : x.message))
        .join("; ");
  }
  if (status === 0) return "The API is unavailable.";
  return `Request failed (${status}).`;
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let r: Response;
  try {
    r = await fetch(path, init);
  } catch {
    throw new ApiError(0, null, "The API is unavailable. Is the orchestration server running?");
  }
  const text = await r.text();
  let body: unknown = null;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    body = text;
  }
  if (!r.ok) {
    if (r.status >= 500 && r.status !== 503 && body === null)
      throw new ApiError(r.status, body, "The API is unavailable.");
    throw new ApiError(r.status, body, messageOf(r.status, body));
  }
  return body as T;
}

const post = <T>(path: string, body: unknown) =>
  request<T>(path, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });

export interface RunBody extends EngineConfig {
  dataset_id: string;
  source_node: number;
  target_node: number;
  trace_level: "none" | "summary" | "visual" | "full";
  max_events?: number;
  repetitions: number;
  warmup: number;
}

export interface LaneComparisonBody {
  dataset_id: string;
  source_node: number;
  target_node: number;
  lanes: { id: string; algorithm: "dijkstra" | "astar"; queue: string; label?: string }[];
  heuristic?: "auto" | "zero" | "haversine" | "euclidean";
  repetitions?: number;
  warmup?: number;
  order_seed?: number;
}

export const api = {
  datasets: () => request<Dataset[]>("/api/datasets"),
  dataset: (id: string) => request<Dataset>(`/api/datasets/${encodeURIComponent(id)}`),
  prepare: (id: string) => post<Job>(`/api/datasets/${encodeURIComponent(id)}/prepare`, {}),
  preparation: (id: string) => request<Job>(`/api/preparations/${id}`),
  queues: () => request<QueueInfo[]>("/api/queues"),
  models: () => request<ModelInfo[]>("/api/models"),
  model: (id: string) => request<ModelInfo>(`/api/models/${encodeURIComponent(id)}`),
  modelNodes: (id: string, nodes: number[]) =>
    request<{
      nodes: Record<string, Record<string, number>>;
      score_semantics: string;
      dataset_id: string;
    }>(`/api/models/${encodeURIComponent(id)}/nodes?nodes=${nodes.join(",")}`),
  capabilities: () => request<Record<string, unknown>>("/api/capabilities"),
  snap: (ds: string, lon: number, lat: number) =>
    post<Snap>(`/api/graphs/${ds}/snap`, { lon, lat }),
  viewport: (ds: string, bbox: [number, number, number, number]) =>
    request<{ segments: number[][]; count: number; too_many: boolean; limit: number }>(
      `/api/graphs/${ds}/viewport?bbox=${bbox.map((v) => v.toFixed(6)).join(",")}`,
    ),
  randomPair: (ds: string, seed: number) =>
    request<{ source_node: number; target_node: number }>(
      `/api/graphs/${ds}/random-pair?seed=${seed}`,
    ),
  async coords(ds: string): Promise<Float32Array> {
    let r: Response;
    try {
      r = await fetch(`/api/graphs/${ds}/coords`);
    } catch {
      throw new ApiError(0, null, "The API is unavailable.");
    }
    if (!r.ok) {
      const body = await r.json().catch(() => null);
      throw new ApiError(r.status, body, messageOf(r.status, body));
    }
    return new Float32Array(await r.arrayBuffer());
  },
  createRun: (body: RunBody) => post<Job>("/api/runs", body),
  runs: (limit = 50) => request<Job[]>(`/api/runs?limit=${limit}`),
  run: (id: string) =>
    request<Job & { has_result: boolean; has_trace: boolean }>(`/api/runs/${id}`),
  cancelRun: (id: string) => post<Job>(`/api/runs/${id}/cancel`, {}),
  runResult: (id: string) => request<RunResult>(`/api/runs/${id}/result`),
  async runTrace(id: string): Promise<TraceEvent[]> {
    const r = await fetch(`/api/runs/${id}/trace`);
    if (!r.ok) {
      const body = await r.json().catch(() => null);
      throw new ApiError(r.status, body, messageOf(r.status, body));
    }
    const text = await r.text();
    const out: TraceEvent[] = [];
    for (const line of text.split("\n")) if (line) out.push(JSON.parse(line) as TraceEvent);
    return out;
  },
  /** Streams a run's engine events over SSE. Returns a closer. */
  streamEvents(
    id: string,
    handlers: {
      onState?: (state: string) => void;
      onBatch: (events: TraceEvent[]) => void;
      onEnd: (info: { state: string; trace?: boolean; error?: { message: string } | null }) => void;
      onError: (message: string) => void;
    },
  ): () => void {
    const es = new EventSource(`/api/runs/${id}/events?batch=4000`);
    let ended = false;
    es.addEventListener("state", (e) =>
      handlers.onState?.(JSON.parse((e as MessageEvent).data).state),
    );
    es.addEventListener("trace", (e) => handlers.onBatch(JSON.parse((e as MessageEvent).data)));
    es.addEventListener("end", (e) => {
      ended = true;
      es.close();
      handlers.onEnd(JSON.parse((e as MessageEvent).data));
    });
    es.onerror = () => {
      if (!ended) {
        es.close();
        handlers.onError("Lost the event stream from the API.");
      }
    };
    return () => es.close();
  },
  createComparison: (body: unknown) => post<Job>("/api/comparisons", body),
  comparison: (id: string) => request<Job>(`/api/comparisons/${id}`),
  comparisons: () => request<Job[]>("/api/comparisons"),
  createLaneComparison: (body: LaneComparisonBody) =>
    post<Job>("/api/lane-comparisons", body),
  laneComparison: (id: string) => request<LaneComparisonJob>(`/api/lane-comparisons/${id}`),
  cancelLaneComparison: (id: string) => post<Job>(`/api/lane-comparisons/${id}/cancel`, {}),
  createBenchmark: (body: unknown) => post<Job>("/api/benchmarks", body),
  benchmarks: () => request<Job[]>("/api/benchmarks"),
  benchmark: (id: string) => request<Job>(`/api/benchmarks/${id}`),
  cancelBenchmark: (id: string) => post<Job>(`/api/benchmarks/${id}/cancel`, {}),
  benchmarkResults: (id: string) => request<ResultSummary>(`/api/benchmarks/${id}/results`),
  researchResults: () => request<ResultListing[]>("/api/research/results"),
  researchResult: (file: string, baseline?: string) =>
    request<ResultSummary>(
      `/api/research/results/${encodeURIComponent(file)}${baseline ? `?baseline=${baseline}` : ""}`,
    ),
  researchNotes: () => request<Note[]>("/api/research/notes"),
};

export async function waitForJob(
  get: () => Promise<Job>,
  onUpdate?: (j: Job) => void,
  intervalMs = 400,
): Promise<Job> {
  for (;;) {
    const j = await get();
    onUpdate?.(j);
    if (j.state === "COMPLETED" || j.state === "FAILED" || j.state === "CANCELLED") return j;
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}
