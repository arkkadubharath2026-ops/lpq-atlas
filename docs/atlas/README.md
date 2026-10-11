# LPQ Atlas

An interactive, reproducible observatory around the existing research engine for
**Dynamic Approximate Shortest Path Using a Learned Priority Queue**. It lets you pick
a real graph and a real query, run Dijkstra or A* over a classical heap or the learned
priority queue (LPQ), watch the engine's own execution trace, compare configurations,
and run the project's benchmark harness from a browser.

The authoritative specification is [`specdoc.md`](../../specdoc.md). This page describes
what was built against it; [DISCREPANCIES.md](DISCREPANCIES.md) tracks each audit item
and the defects found while building.

## What is real, and how you can tell

| On screen | Where it comes from |
|---|---|
| Distance, path, operation counters, LPQ statistics | the `lpq_run` result JSON (C++), unmodified |
| Benchmark runtime | `run_benchmark` over **untraced** repetitions inside `lpq_run` |
| Animated exploration | the JSONL trace of a *separate* instrumented run of the same query |
| "trace reproduces the measured run" | `lpq_run` compares the traced and untraced runs (distance, path, every counter, LPQ metrics) |
| Exact / ratio badge | checked against `reference_dijkstra_heap` in the same process |
| Snapped source/target | nearest node of the processed graph (not the tile layer) |
| Benchmark workspace | `lpq_benchmark --experiment` (one pinned process, paired queries, each validated) |
| Research charts | stored `results/raw/*.json`, summarised with `lpq_tools.statistics` |
| Model inputs and scores | `lpq_model_info --nodes`, i.e. the engine's `GraphFeatures` + `StaticNodeScorer` |
| Hardware counters | shown as unavailable: no PMU on the development host; never estimated |

## Architecture

```
atlas/web (Next.js, TypeScript, Leaflet)
   │  REST + server-sent events (same-origin /api rewrite)
atlas/api (FastAPI)  — validation, registry, file-backed jobs, trace streaming
   │  argv lists built from enums (no shell)          │ imports
cpp/apps/lpq_run, lpq_benchmark, lpq_export,          python/lpq_tools (datasets, OSM,
         lpq_model_info  (the engine)                  statistics, leakage guard)
   │
datasets/ (OSM, DIMACS, SNAP, synthetic) · models/ · results/raw · results/atlas (jobs)
```

* **Engine seam.** `dijkstra()` and `astar()` take an optional observer template
  parameter ([search_observer.hpp](../../cpp/include/lpq/algorithms/search_observer.hpp)).
  The default `NullSearchObserver` compiles every hook out, so the benchmarked code is
  unchanged. The LPQ is observed through its existing `ExtractionObserver` interface; the
  recorder ([trace_recorder.hpp](../../cpp/include/lpq/trace/trace_recorder.hpp)) returns
  an uncapped policy (or forwards the adaptive controller's), so observation does not
  alter extraction. `test_trace` and every `lpq_run` verify this.
* **Persistence** is filesystem-only (spec §24): each run/comparison/benchmark is a
  directory in `results/atlas/` with `request.json`, `status.json`, the engine's
  `result.json` / `trace.jsonl` / `results.json`, and `engine.log`. Jobs states:
  `QUEUED → PREPARING → RUNNING → FINALIZING → COMPLETED | FAILED | CANCELLED`.
* **Scale modes.** `teaching` (≤ 20k nodes, full visual trace), `route` (≤ 400k nodes with
  coordinates; the trace is capped and the cap is reported), `research` (no coordinates or
  larger; node ids and metrics, no map).

## Datasets

| id | family | nodes | notes |
|---|---|---:|---|
| `osm_monaco` | OSM | 5,692 | Geofabrik snapshot 2026-09-25, SHA-256 pinned |
| `osm_liechtenstein` | OSM | 29,945 | Geofabrik snapshot 2026-09-25, SHA-256 pinned |
| `dimacs_ny` / `col` / `fla` | DIMACS | 264k / 436k / 1.07M | coordinates, route/research mode |
| `snap_facebook`, `snap_roadnet_pa`, `snap_roadnet_ca` | SNAP | 4k – 2M | no coordinates |
| `synth_ratio1_ny` | synthetic | 264k | NY topology, synthesized weights |

OSM extraction ([osm.py](../../python/lpq_tools/data/osm.py)) documents its road-access
policy in full: drivable `highway` classes, `access`/`motor_vehicle` exclusions, one-way
semantics (explicit and implied), bridges/tunnels via shared nodes only, largest strongly
connected component, **no turn restrictions**. Weights are great-circle decimetres, so a
route is the *shortest*, never the *fastest*. The extractor writes DIMACS, and the
existing `lpq_convert` produces the graph, so OSM uses the same converter and checksum
path as every other dataset. Provenance is in `datasets/metadata/<id>.osm.json`.

```bash
python scripts/data/prepare_datasets.py osm_monaco osm_liechtenstein
```

> The configs pin dated Geofabrik snapshots. Geofabrik prunes old dated extracts: the
> `monaco-260925` file returned **HTTP 404** in CI on 2026-10-11, so these two datasets
> cannot be prepared until a current snapshot is chosen and its SHA-256 re-pinned. The
> end-to-end CI job therefore uses the bundled offline `demo_road` graph instead.

## Running it

A step-by-step user guide is in [INSTRUCTION_MANUAL.md](../../INSTRUCTION_MANUAL.md).

**Containers.** `docker compose up --build` builds two images from
[atlas/Dockerfile](../../atlas/Dockerfile): `api` compiles the engine from source on
Debian bookworm and runs FastAPI; `web` is the production Next.js build, with `/api`
rewritten to the `api` service at build time. `datasets/` and `experiments/` are mounted
read-write (Prepare writes there), `models/` and `results/` read-only; container jobs go
to the `atlas-work` volume, separate from a host API's `results/atlas/`.

**From source.** Prerequisites: a C++20 compiler + CMake + Ninja, Python 3.13, Node 22.

```bash
cmake --preset release && cmake --build build/release        # engine (from source)
pip install -r python/requirements-dev.txt -r atlas/api/requirements.txt
python scripts/demo/prepare_demo.py                            # bundled demo_road, offline
# optional: python scripts/data/prepare_datasets.py osm_monaco   # needs a live Geofabrik snapshot (see below)

python -m uvicorn atlas_api.app:app --app-dir atlas/api --port 8000
cd atlas/web && npm ci && npm run build && npx next start -p 3000
# open http://127.0.0.1:3000
```

`next dev` also works; `ATLAS_API_URL` points the UI at another API address.
API environment: `ATLAS_MAX_JOBS` (2), `ATLAS_RUN_TIMEOUT_S` (180),
`ATLAS_MAX_TRACE_EVENTS` (400,000), `ATLAS_WORK_DIR` (`results/atlas`).

## Tests

| suite | command | what it proves |
|---|---|---|
| C++ (GoogleTest) | `ctest --test-dir build/release` | engine correctness incl. `test_trace` (observed = unobserved, events replay to the result, exact drop counts) and the overflow regressions |
| Fuzz smoke | label `fuzz-smoke` in ctest; `-DLPQ_FUZZ_ENGINE=libfuzzer` with clang | parsers and queue invariants never crash on arbitrary input |
| Python | `cd python && pytest` | dataset pipeline, OSM policy + deterministic extraction, leakage guard, statistics |
| API contract | `cd atlas/api && pytest` | API result ≡ direct `lpq_run` result for every queue, exact and ε-approximate LPQ, A*, unreachable pairs, malformed requests, SSE ≡ trace file |
| UI unit | `cd atlas/web && npm test` | replay state machine |
| End-to-end | `cd atlas/web && npx playwright test` | map selection → Dijkstra → LPQ → metrics equal API → replay → compare → unreachable → backend failure |

CI: [.github/workflows/ci.yml](../../.github/workflows/ci.yml).

## Timing methodology (spec §26)

The benchmark clock covers one untraced search per sample. **Excluded**: graph loading,
queue allocation and sizing, heuristic-scale derivation, model score precomputation
(reported separately as `scorer_build_ns`), the reference search, trace recording and all
serialization. Samples are pinned to one core where the OS allows it and summarised by
median and MAD; noisy measurements are flagged, never dropped. The instrumented trace
run's wall clock is reported separately as `instrumented_trace_run` and is never shown
as the algorithm's runtime. Memory is not reported: no per-query measurement method is
defined for `lpq_run`.

## Security (spec §36)

No shell anywhere: every engine call is an argv list built from enums and integers.
Dataset, model and manifest identifiers are regex-validated and resolved against the
registry's directory listing, never joined into paths. Trace size, event count, job
concurrency and run time are capped. Validation errors return field names and messages
only; engine failures return a category and a message without host paths.

## Limitations

* Turn restrictions are not modelled in OSM graphs; weights are distances.
* Route-mode traces on large graphs are capped (the UI says how many events were not
  recorded); the result and metrics are always complete.
* The static model scorer is the only learned configuration benchmarked; MLP and tree
  ensemble predictors exist in the engine but have no trained artifacts.
* Hardware counters are unavailable on the development host.
