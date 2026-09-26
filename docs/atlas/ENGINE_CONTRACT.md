# Engine contract: `lpq_run`, traces, and helper tools

The machine-readable interface the web layer drives (spec §7–§9). Source:
[cpp/apps/lpq_run.cpp](../../cpp/apps/lpq_run.cpp).

## `lpq_run`

```
lpq_run --dataset <file.lpqgraph> --source <n> --target <n>
        [--algorithm dijkstra|astar] [--heuristic auto|zero|euclidean|manhattan|haversine]
        [--pq binary_heap|four_ary_heap|pairing_heap|fibonacci_heap|radix_heap|lpq|lpq_model|lpq_adaptive]
        [--model <file>] [--adaptive-learning] [--mode exact|approximate] [--epsilon e]
        [--lpq-window lo:hi] [--lpq-bucket-width n]
        [--trace-level none|summary|visual|full] [--max-events n] [--trace-out file]
        [--repetitions n] [--warmup n] [--cpu-core n] [--run-id s] [--result-out file]
```

Every combination is validated before any work: approximate mode only for learned
queues and with 0 < ε ≤ 1; `--model` only with `lpq_model`; LPQ options only for learned
queues; `--heuristic auto` resolves to `haversine` on geographic graphs and `euclidean` on
plane graphs and is refused when the graph has no coordinates. Exit codes: 0 completed,
64 invalid request, 1 other failure. A failure still writes a result with
`status: "failed"` and `error.category` ∈ {`invalid_request`, `unsupported`, `dataset`,
`model`, `io`, `engine`}.

Execution order: load graph → derive heuristic scale → build model scores → reference
search → **authoritative untraced run** → traced run (if any) → timed untraced runs.

### Result (schema_version 1)

| field | meaning |
|---|---|
| `request` | the validated parameters, as executed |
| `dataset` | id, nodes, edges, arcs, directed, `checksum_fnv1a64` (string), coordinate system, weight range |
| `result` | `reachable`, `termination`, `distance` (null if unreachable), `path`, `path_weights`, `path_hops`, `path_cost_verified` |
| `reference` | `reference_dijkstra_heap`'s reachability and distance |
| `correctness` | policy, ε, `exact_match`, `approximation_ratio` (null if unreachable), `bound`, `bound_holds`, `reachability_agrees` |
| `counters` | pushes, pops, decrease-keys, comparisons (incl. LPQ verification), stale pops, settled, edges examined/relaxed, peak queue; null if instrumentation is compiled out |
| `heuristic` | name, scale, how the scale was derived |
| `lpq` | bucket width, window, scorer kind (`constant_control_no_model` / `static_precomputed_model_scores`), adaptive controller, online learning, model file, `scorer_build_ns`, metrics, candidate / verification / correction counters |
| `timing` | `clock_boundary` (text), `benchmark` (samples, median, MAD, min, max, noise, pinning, warnings), `phases_ns` (graph load, heuristic scale, scorer build, reference search, instrumented trace run) |
| `trace` | level, availability, events written/dropped, truncated, `matches_untraced_run` |
| `environment`, `hardware_counters` | build and host provenance; counters are reported unavailable, never estimated |

## Trace (JSONL, schema_version 1)

One JSON object per line with `schema_version`, a gap-free `seq` starting at 1, and `type`.

| type | level | fields |
|---|---|---|
| `RUN_STARTED` | all | algorithm, queue, source, target, trace_level, max_events |
| `SOURCE_INITIALIZED` | visual+ | node, distance, (A*: h, f) |
| `NODE_SETTLED` | visual+ | node, distance |
| `EDGE_RELAXED` | visual+ | from, to, old_distance (null = unreached), new_distance, key, (A*: h) |
| `QUEUE_POP` / `STALE_POP` | full | node, key, (queue_size) |
| `QUEUE_PUSH` / `QUEUE_DECREASE_KEY` | full | node, key |
| `EDGE_EXAMINED` | full | from, to, weight, candidate |
| `LPQ_BUCKET_SELECTED` | visual+ | bucket_base, bucket_width, bucket_size, bound, queue_size |
| `LPQ_CANDIDATE_WINDOW` | visual+ | front[{node,key,score}] (≤ 8 visual, ≤ 32 full), front_shown |
| `LPQ_CANDIDATE_SELECTED` | visual+ | prediction_correct, rank_displacement, predicted_key, (node) |
| `LPQ_VERIFICATION_ACCEPTED` | visual+ | node, key, comparisons, entries_skipped, resolution |
| `LPQ_VERIFICATION_REJECTED` | visual+ | reason, attempts / attempts_before_accept |
| `LPQ_CORRECTION_APPLIED` | visual+ | kind (`window_expanded` / `reordered_within_window`), expansions, rank/key displacement |
| `LPQ_FALLBACK_STARTED` | visual+ | reason (`verification_failed`, `empty_window`, `empty_front`, `adaptive_controller_retreat`) |
| `LPQ_ADAPTIVE_POLICY` | visual+ | use_learned_path, window_size (on change) |
| `DESTINATION_REACHED` | all | node, distance |
| `PATH_RECONSTRUCTED` | all | path, distance |
| `RUN_COMPLETED` | all | termination, reachable, distance, events_written, events_dropped, truncated |

Past `--max-events`, events are dropped and counted exactly; the three closing events are
always written. Invariants tested in `cpp/tests/trace/test_trace.cpp` and
`atlas/api/tests/test_api_contract.py`: sequence gap-free; `NODE_SETTLED` count equals
`settled_nodes`; replaying `EDGE_RELAXED` reproduces every final distance; the
reconstructed path equals the result path; LPQ accept/fallback counts equal the queue's
own counters.

## `lpq_export`

`lpq_export --dataset <file> --out-dir <dir>` writes `coords.f64` (n×2, lon/lat or x/y),
`offsets.u64`, `targets.u32`, `weights.u32`, optional `original_ids.u64`, and
`export.json` (counts, checksum, coordinate system). Used by the API for snapping and
map drawing, so the `.lpqgraph` format keeps a single reader.

## `lpq_model_info --nodes`

`lpq_model_info --model <m> --dataset <g> --nodes a,b,c` prints each node's six static
features and the score `StaticNodeScorer` assigns it — the exact values the learned queue
stores at push time.
