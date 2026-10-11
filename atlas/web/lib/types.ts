// Shapes of what the API returns. The run result mirrors lpq_run's JSON
// (schema_version 1); nothing here is computed by the UI.

export type JobState =
  | "QUEUED"
  | "PREPARING"
  | "RUNNING"
  | "FINALIZING"
  | "COMPLETED"
  | "FAILED"
  | "CANCELLED";

export const TERMINAL: JobState[] = ["COMPLETED", "FAILED", "CANCELLED"];

export interface Job {
  id: string;
  kind: string;
  state: JobState;
  created_at: number;
  updated_at: number;
  error: { category: string; message: string } | null;
  progress: string | null;
  summary: Record<string, unknown>;
  request?: Record<string, unknown>;
}

export interface Manifest {
  file: string;
  role: "training" | "evaluation";
  seed: number;
  query_count: number;
}

export interface Dataset {
  id: string;
  family: string;
  status: string;
  status_detail?: string;
  description?: string;
  nodes?: number;
  edges?: number;
  arcs?: number;
  directed?: boolean;
  weighted?: boolean;
  min_weight?: number;
  max_weight?: number;
  mean_weight?: number;
  has_coordinates?: boolean;
  coordinate_system?: "none" | "euclidean" | "geographic";
  checksum_fnv1a64?: string;
  source_format?: string;
  source_urls?: string[];
  provenance?: Record<string, unknown>;
  weights_synthesized?: boolean;
  normalization?: Record<string, string>;
  query_manifests: Manifest[];
  compatible_algorithms: string[];
  astar_heuristics: string[];
  scale_mode: "teaching" | "route" | "research";
  models: string[];
  osm?: {
    region?: string;
    snapshot_date?: string;
    source_url?: string;
    source_sha256?: string;
    licence?: string;
    road_policy_version?: number;
    weight_unit?: string;
    turn_restrictions_modelled?: boolean;
    ways_kept?: number;
    ways_excluded?: Record<string, number>;
    nodes_dropped_outside_largest_scc?: number;
    bbox?: [number, number, number, number];
    extractor?: string;
  };
}

export interface QueueInfo {
  id: string;
  name: string;
  family: "classical" | "learned";
  decrease_key?: boolean;
  scorer?: string;
  requires_model?: boolean;
  description?: string;
  /** True/false when the installed engine says whether it provides this queue; null if unknown. */
  engine_available?: boolean | null;
  exact?: boolean;
}

export interface SplitReport {
  status: "DISJOINT" | "OVERLAPPING" | "INCOMPARABLE";
  training_manifest: string;
  evaluation_manifest: string;
  training_queries: number;
  evaluation_queries: number;
  shared_pairs: number;
  shared_endpoints: number;
  detail: string;
}

export interface ModelInfo {
  id: string;
  file: string;
  sha256: string;
  size_bytes: number;
  kind?: string;
  feature_layout_version?: string;
  feature_count?: number;
  native_weights?: Record<string, number>;
  used_features?: string[];
  loader_ok?: boolean;
  training_dataset?: string;
  training_manifest?: string;
  splits: SplitReport[];
  split_status: string;
  mode: string;
  mode_explanation: string;
  report: Record<string, unknown>;
}

export interface Snap {
  node: number;
  lon: number;
  lat: number;
  clicked: { lon: number; lat: number };
  snap_distance_m: number;
  out_degree: number;
  osm_node_id?: number;
}

export interface EngineConfig {
  algorithm: "dijkstra" | "astar";
  heuristic: "auto" | "zero" | "haversine" | "euclidean";
  queue: string;
  correctness_policy: "exact" | "approximate";
  epsilon: number;
  model_id: string | null;
  adaptive_learning: boolean;
}

export interface Counters {
  pq_pushes: number;
  pq_pops: number;
  pq_decrease_keys: number;
  pq_comparisons: number;
  stale_pops: number;
  settled_nodes: number;
  edges_examined: number;
  edges_relaxed: number;
  peak_queue_size: number;
}

export interface LpqResult {
  bucket_width: number;
  window: [number, number];
  scorer: string;
  adaptive_controller: boolean;
  online_learning: boolean;
  model_path_name?: string;
  scorer_build_ns?: number;
  scorer_memory_bytes?: number;
  features?: string[];
  metrics: {
    extractions: number;
    verified_extractions: number;
    fallback_scans: number;
    entries_examined: number;
    entries_skipped: number;
    window_divergences: number;
    verified_rate: number;
    fallback_rate: number;
    mean_entries_skipped: number;
  };
  candidate: Record<string, number>;
  verification: Record<string, number>;
  correction: Record<string, number>;
}

export interface RunResult {
  schema_version: number;
  run_id: string;
  status: string;
  generated_at: string;
  request: {
    algorithm: string;
    heuristic: string | null;
    queue: string;
    mode: string;
    epsilon: number;
    source: number;
    target: number;
    trace_level: string;
    max_events: number;
    repetitions: number;
    warmup: number;
    adaptive_learning: boolean;
  };
  dataset: {
    id: string;
    nodes: number;
    edges: number;
    arcs: number;
    directed: boolean;
    checksum_fnv1a64: string;
    coordinate_system: string;
    min_weight: number;
    max_weight: number;
  };
  result: {
    reachable: boolean;
    termination: string;
    distance: number | null;
    path: number[];
    path_weights: number[];
    path_hops: number;
    path_cost_verified: boolean;
  };
  reference: { algorithm: string; reachable: boolean; distance: number | null };
  correctness: {
    policy: string;
    epsilon: number;
    reachability_agrees: boolean;
    exact_match: boolean;
    approximation_ratio: number | null;
    bound: number;
    bound_holds: boolean;
  };
  counters_available: boolean;
  counters: Counters | null;
  heuristic: { name: string; scale: number; scale_derivation: string; consistent: boolean } | null;
  lpq: LpqResult | null;
  timing: {
    clock_boundary: string;
    benchmark: {
      repetitions: number;
      warmup: number;
      samples_ns: number[];
      median_ns: number;
      mad_ns: number;
      min_ns: number;
      max_ns: number;
      relative_mad: number;
      noisy: boolean;
      pinned: boolean;
      core: number;
      warnings: string[];
    } | null;
    phases_ns: Record<string, number | null>;
  };
  trace: {
    level: string;
    available: boolean;
    events_written?: number;
    events_dropped?: number;
    truncated?: boolean;
    matches_untraced_run?: boolean;
    mismatch?: string;
    removed_reason?: string;
  };
  environment: Record<string, string | number | boolean>;
  hardware_counters: { available: boolean; reason: string };
}

export interface ConfigSummary {
  key: string;
  algorithm: string;
  pq: string;
  mode: string;
  queries: number;
  supported: boolean;
  unsupported_reason: string | null;
  median_of_medians_ns?: number;
  mad_of_medians_ns?: number;
  per_query_median_ns?: number[];
  within_query_noise?: number;
  total_comparisons?: number;
  total_pops?: number;
  total_settled?: number;
  total_relaxations?: number;
  exact_matches?: number;
  unreachable?: number;
  max_approximation_ratio?: number | null;
  median_approximation_ratio?: number | null;
  all_pinned?: boolean;
  noisy_queries?: number;
}

export interface PairedComparison {
  config: string;
  baseline: string;
  pairs: number;
  median_ratio_vs_baseline: number | null;
  median_difference_ns: number;
  difference_ci_ns: [number, number];
  p_value: number;
  adjusted_p_value: number | null;
  correction: string | null;
  test: string;
  rank_biserial: number;
  fraction_baseline_faster: number;
  ties: number;
}

export interface ResultSummary {
  file: string;
  experiment_id: string;
  generated_at: string;
  dataset: string | null;
  graph_nodes: number | null;
  records: number;
  environment: Record<string, string | number | boolean>;
  hardware_counters: Record<string, unknown> | null;
  configurations: ConfigSummary[];
  fastest_by_median: string | null;
  baseline: string | null;
  paired_comparisons: PairedComparison[];
}

export interface ResultListing {
  file: string;
  experiment_id: string;
  generated_at: string;
  dataset_nodes: number | null;
  records: number;
  configurations: string[];
  fastest_by_median: string | null;
  error?: string;
}

export interface Note {
  id: string;
  title: string;
  markdown: string;
}

// ---- Quick Compare (POST /api/lane-comparisons) -------------------------------
// Every figure below is produced by the API from the engine's own output; the UI
// only formats it. `speedup_vs_vanilla` is vanilla median / lane median.

export type LaneVerdict = "faster" | "slower" | "same" | "reference" | "unavailable";
export type LaneEffectKind = "vanilla" | "queue" | "ml";
export type VerdictReason = "within_noise" | "not_consistent_across_rounds" | "single_round" | null;

export interface LaneTiming {
  /** Median of the per-round medians. */
  median_ns: number;
  /** Spread: the larger of the within-process MAD and the MAD of the per-round medians. */
  mad_ns: number;
  mad_within_ns?: number;
  mad_between_rounds_ns?: number;
  /** One entry per round (null where that round failed). */
  round_medians_ns?: (number | null)[];
  rounds?: number;
  rounds_ok?: number;
  relative_mad: number | null;
  min_ns: number | null;
  max_ns: number | null;
  samples_ns: number[];
  repetitions: number | null;
  warmup: number | null;
  noisy: boolean | null;
  pinned: boolean | null;
  core: number | null;
  warnings: string[];
}

export interface LaneCounts {
  pops: number | null;
  pushes: number | null;
  decrease_keys: number | null;
  comparisons: number | null;
  settled_nodes: number | null;
  stale_pops: number | null;
  edges_examined: number | null;
  edges_relaxed: number | null;
  peak_queue_size: number | null;
}

export interface LaneResult {
  id: string;
  label: string | null;
  algorithm: "dijkstra" | "astar";
  queue: string;
  family: string | null;
  /** Position in each round's shuffled order. */
  run_positions: number[];
  run_id: string;
  status: "ok" | "error";
  error: { category: string; message: string } | null;
  timing: LaneTiming | null;
  counts?: LaneCounts;
  distance?: number | null;
  reachable?: boolean;
  path_hops?: number | null;
  heuristic_used?: string | null;
  lpq?: Record<string, unknown> | null;
  matches_reference: boolean | null;
  effect: LaneEffectKind;
  vanilla_lane_id: string | null;
  speedup_vs_vanilla: number | null;
  verdict: LaneVerdict;
  within_noise: boolean | null;
  verdict_reason?: VerdictReason;
  /** Rounds (of `rounds_compared`) in which this lane was ahead of / behind its vanilla lane. */
  rounds_faster?: number | null;
  rounds_slower?: number | null;
  rounds_compared?: number | null;
  rank: number | null;
  vs_fastest: number | null;
  /** From the engine's `optimizer` block, for the Optimized queue. */
  resolved_queue?: string | null;
  resolved_kind?: string | null;
  resolution_method?: "autotune" | "autotune_cached" | "policy" | "default" | null;
  /** One-time set-up cost of the untimed call before round 1; not part of any timed figure. */
  calibration_ns?: number | null;
  resolution_reason?: string | null;
  predicted_ratio_vs_radix?: number | null;
  resolved_to_radix?: boolean;
  failed_rounds?: number[];
}

export interface LaneEffect {
  kind: "algorithm" | "queue" | "ml" | "ml_vs_best_classical";
  lane_id: string;
  versus_lane_id: string;
  speedup: number | null;
  verdict: LaneVerdict;
  reason?: VerdictReason;
  rounds_faster?: number | null;
  rounds_slower?: number | null;
  rounds_compared?: number | null;
}

export interface LaneComparisonResult {
  schema_version: number;
  dataset_id: string;
  source_node: number;
  target_node: number;
  measurement: {
    rounds: number;
    repetitions: number;
    warmup: number;
    tracing: string;
    execution: string;
    /** The shuffled lane order of each round. */
    round_orders: string[][];
    order_seed: number;
    one_time_setup_ns?: number;
    all_pinned: boolean;
    cores: number[];
    clock: string;
    machine: { os: string; arch: string };
    elapsed_s: number;
    contended_lanes: string[];
  };
  heuristic: { requested: string; used_by_astar_lanes: string | null; shared: boolean };
  lanes: LaneResult[];
  path: number[];
  fastest: {
    lane_id: string;
    median_ns: number;
    tied_with: string[];
    clear_win: boolean;
  } | null;
  correctness: {
    checked_lanes: number;
    failed_lanes: string[];
    references_agree: boolean;
    all_lanes_same_distance: boolean | null;
    all_match_reference: boolean | null;
    mismatching_lanes: string[];
    reference_distance: number | null;
    reachable: boolean | null;
  };
  effects: LaneEffect[];
  noise_rule: string;
  vanilla_queue: string;
}

export interface LaneComparisonJob extends Job {
  result: LaneComparisonResult | null;
}
