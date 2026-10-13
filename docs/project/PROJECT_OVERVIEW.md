# LPQ Atlas — Project Overview (end to end)

**Research title:** Dynamic Approximate Shortest Path Using a Learned Priority Queue (LPQ)
**Product name:** LPQ Atlas (interactive observatory around the research engine)
**Repository:** `arkkadubharath2026-ops/lpq-atlas`
**Document purpose:** explain what the project is, how it is built, what each part does,
and how the pieces fit together. This is an orientation document. For completion status
and remaining work, read [PROJECT_STATUS.md](PROJECT_STATUS.md).

**Evidence labels used in this document**

| label | meaning |
|---|---|
| **[verified here]** | I ran it in this session (build, test, or query) |
| **[doc]** | taken from a project document; not re-run in this session |
| **[PR #1]** | from the open draft PR `claude/affectionate-gates-53lchk` (v2 work); the code is on that branch, not on `main` |

Two states of the research repository matter, and they are not the same:

* **`main` (commit `4e7ad00`)** — the "v1" research state: the original engine, the first
  measurements, and LPQ Atlas v1. This document was first written from this checkout.
* **PR #1 branch (head `e6dde1c`, 21 commits ahead of `main`)** — the "v2" state: an
  autotuned exact queue (`lpq_optimized`), a reworked LPQ extraction path, a queue-policy
  trainer, Quick Compare in the UI, local setup scripts, and a results report
  (`docs/experiments/v2-results.md`) that records the audit's retraction of a v1 claim. It is
  an open **draft** PR and is **not merged**.

A third, local-only branch, `completion/v2-baseline`, is `e6dde1c` plus this document pair
and the M2 documentation corrections. It has not been pushed.

Where the states disagree, this document says so.

> **Revision note (M2, 2026-10-13).** The first version of this document said the PR #1
> documents supersede the v1 34% adaptive-controller claim. **That was wrong**: v2 edits none
> of the four documents that state it. The claim is **historical and retracted**, and is
> annotated as such on this branch only. The §10 table also now carries the M1 baseline for
> the v2 head and the hosted CI results, including the failing run #28. See
> [PROJECT_STATUS.md](PROJECT_STATUS.md), revision note, for the full list of changes.

---

## 1. One-paragraph summary

LPQ Atlas is a shortest-path research system. Its engine is native C++20: it computes
single-pair shortest paths with Dijkstra or A* over a compressed road or social graph,
using a choice of classical priority queues (binary, 4-ary, pairing, Fibonacci, radix)
or a *learned priority queue* (LPQ). The LPQ uses a learned scorer only as a *hint* for
which candidate to extract next; every extraction is *verified* against a correctness
rule, corrected when wrong, and falls back to a classical bucket scan when verification
fails. The project's question is whether a learned, verified queue can cut enough
queue work to pay for the machinery that keeps it safe, measured end to end on real
road and social graphs. The current answer, from the project's own measurements, is
**mostly no**: the learned component gives little or no measured gain, and most of the
benefit comes from the queue substrate. The project's value is the measurement
infrastructure, the correctness discipline, and an interactive system that replays the
real engine trace and makes each claim checkable. Around the engine sits LPQ Atlas: a
FastAPI backend that runs the engine and a Next.js front end that shows maps, search
traces, comparisons, benchmarks, datasets and models.

---

## 2. The research question and hypothesis

### 2.1 Core hypothesis [doc: PRD §5]

A lightweight learned model can predict useful local ordering information about the
shortest-path frontier well enough that a verified, adaptive queue reduces expensive
queue work and improves **end-to-end** runtime, while staying robust when predictions
degrade. The hypothesis counts as supported only if: end-to-end runtime improves after
all ML overhead is included; the gains are statistically reproducible; the result does
not depend on one graph or one source/target pair; degradation under distribution shift
is measured; classical fallback prevents catastrophic behaviour; and accuracy/cost
trade-offs are characterised.

### 2.2 Prior art and novelty boundary [doc: PRD §2, README §2]

A 2024 NeurIPS paper, *Learning-Augmented Priority Queues* (Benomar and Coester),
already studies learning-augmented priority queues and applies rank predictions to
Dijkstra. This project must **not** claim learned priority queues are novel in
themselves. Its intended contribution is the system-level integration: online
adaptation from search-time signals, confidence-aware candidate extraction,
verification and correction inside a practical queue, adaptive switching, a unified
exact and bounded-approximate framework, graph-distribution generalisation, and
end-to-end wall-clock accounting. A literature note exists on PR #1
(`docs/research/literature-and-novelty.md`); a complete fresh literature review is still
listed as outstanding in the status document.

### 2.3 Operating modes [doc: PRD §4]

| mode | contract |
|---|---|
| **Exact** | returned cost equals the trusted reference cost; the learned component may steer the search but can never finalise an unverified candidate |
| **Bounded approximate** | user supplies ε > 0; target contract `cost_returned ≤ (1+ε)·cost_optimal`, checked against the reference on every query; a run aborts if the bound is broken |
| **Adaptive** | the queue chooses among learned extraction, narrow verified window, wider window, and classical fallback, from observed prediction quality and a policy |

The project is explicit that an empirical approximation must never be labelled a formal
guarantee. No formal proof of the correction policy exists in the repository.

---

## 3. The core idea: prediction is a hint, verification is the authority

The queue follows this pipeline [doc: PRD §3, §7; abstract]:

```text
Prediction → Candidate selection → Verification → Extraction → Correction / learning feedback
```

Concretely, in the v1 engine [doc: docs/atlas/ABSTRACT.md, docs/atlas/ENGINE_CONTRACT.md]:

1. **Bucket substrate.** Entries are stored in key-bucketed structures with a bucket width.
   A bucket is a range of keys; entries in the front of the active bucket are the candidate
   pool. Keys beyond the window go into an overflow list and are re-filed when the window
   advances.
2. **Static scorer.** A model is evaluated once per node, when the node is inserted, on
   static graph features (six features in the deployed scorer). The score orders entries
   inside a bucket. (The engine defines a 28-feature vector; the deployed scorer uses six
   of them. [verified here: `kFeatureCount` in `cpp/include/lpq/features/feature_vector.hpp`])
3. **Candidate window.** The best-scored entries form a window. The window size is
   bounded by a configured maximum and grows with queue size, from a starting policy.
4. **Verification.** The extracted candidate is checked against a valid lower bound on
   the true minimum key. If the check fails, the window is widened and the check repeats.
5. **Correction / fallback.** If widening does not establish correctness, the queue falls
   back to a classical bucket scan. The fallback count is recorded.
6. **Learning feedback (designed, not active on `main`).** Verified outcomes can produce
   `PredictionEvent`s for an online learner. See §5.5 for why this is not operating.

The point of the design is that the learned part is allowed to be wrong. Correctness
comes from verification and fallback, not from the model.

---

## 4. Repository layout (verified)

Commit `4e7ad00` tracks **313 files**. Sizes below were counted in this session.

| area | path | contents | size |
|---|---|---|---|
| Engine headers | `cpp/include/lpq/` | graph, algorithms, pq, lpq, features, models, verification, adaptation, metrics, trace, common | ~34.5k lines C++ (headers and sources, `cpp/`) |
| Engine apps | `cpp/apps/` | 7 executables: `lpq_run`, `lpq_benchmark`, `lpq_profile`, `lpq_collect`, `lpq_convert`, `lpq_export`, `lpq_model_info` | |
| Engine tests | `cpp/tests/` | GoogleTest suites by module | 847 tests in the release build [verified here] |
| Fuzzing | `cpp/fuzz/` | 4 libFuzzer targets + standalone driver: graph binary reader, model loader, experiment manifest, queue operation sequences | |
| Python tooling | `python/lpq_tools/` | `data`, `training`, `evaluation`, `statistics`, `plotting`, `utilities` | ~8.2k lines Python |
| Python tests | `python/tests/` | 12 test modules | 222 passed, 8 skipped [verified here] |
| Atlas API | `atlas/api/` | FastAPI app: registry, jobs, engine wrapper, graph cache, SSE trace streaming, logging | ~2.5k lines Python |
| Atlas API tests | `atlas/api/tests/` | API contract tests (API result equals direct engine result) | 26 passed [verified here] |
| Atlas web | `atlas/web/` | Next.js app, TypeScript, Leaflet map, Playwright e2e | ~4k lines TS/TSX/CSS |
| Configs | `configs/` | `datasets/` (8 YAML), `experiments/` (4 YAML) | |
| Manifests | `experiments/manifests/` | 16 query manifests (fixed seeds, source/target pairs) | |
| Datasets metadata | `datasets/metadata/` | 22 metadata/checksum/provenance JSON files; **raw and processed graphs are not tracked** | |
| Docs | `docs/` | `docs/atlas/` (engine contract, discrepancy register, decisions, abstract), `docs/experiments/` (measured records), `docs/reproducing.md` | |
| Research summary | `experiments/paper/findings.md` | synthesis of the measured results (v1) | |
| Scripts | `scripts/` | `build/`, `data/`, `profile/`, `reproduce/` | |
| CI | `.github/workflows/ci.yml` | workflow file present on `main`; the hosted runs I could read are all on PR #1, where 5 jobs are green (see §10) | |
| Spec documents | `PRD.md`, `ESD.md`, `README.md`, `CLAUDE.md`, `specdoc.md` | authoritative specifications (see §12) | |

Not tracked and regenerated locally (`.gitignore`): `build/`, `datasets/raw/`,
`datasets/processed/`, `models/checkpoints/`, `models/exported/`, `results/raw/`,
`results/processed/`, `results/figures/`, `results/tables/`, `results/atlas/`,
`graphify-out/`.

---

## 5. The C++ engine, module by module

### 5.1 Graph layer (`cpp/include/lpq/graph/`)

* **CSR graph** [doc: ESD §5]. Offsets, edges and weights arrays. Supports directed
  graphs; undirected graphs are stored as paired arcs. Non-negative integer weights.
* **Binary graph format.** Version 3 stores the coordinate *system* as well as the
  coordinate array. Version 2 still loads. A defect in v1 is recorded here: version 2 stored
  a coordinate *flag*, so every graph loaded from disk reported no coordinates and A* was
  unreachable from a saved graph. This is fixed and has regression tests [doc:
  `docs/experiments/astar.md`, `docs/atlas/DISCREPANCIES.md` defect 6 and §3 defects].
* **Readers and converters.** SNAP and DIMACS readers, a checksummed binary writer, a
  query-manifest reader, graph metadata (node/arc counts, weight statistics), a synthetic
  generator for six families (grid, Erdős–Rényi, Barabási–Albert, random geometric, a
  Poisson-Voronoi *approximation*, road-like), and weight distributions. [verified here:
  the header documents the six families; the Poisson-Voronoi generator is flagged in its own
  source comment as an approximation to read before citing.]
* **Input validation.** The binary reader checks declared counts against the bytes
  remaining before allocating (fixed after a libFuzzer finding; see §5.9).

### 5.2 Shortest-path algorithms (`cpp/include/lpq/algorithms/`)

* **Reference Dijkstra** (`reference_dijkstra.hpp`). Deliberately simple: it uses a linear
  scan over an array for the minimum, so it is slow but easy to audit. It is the **trusted
  reference** that every other result is compared against. It also serves as the "plain
  array / linear scan" baseline that the PRD asks for on small graphs. [verified here:
  `ReferenceOracles.LinearScanAndStdHeapAgreeOnAllDistances` is in the test suite]
* **Optimised Dijkstra** (`dijkstra.hpp`). Templated over the queue type; supports
  single-target early exit and full single-source mode; records a predecessor array and
  settled state; has an observer hook that compiles to nothing by default.
* **A\*** (`astar.hpp`) with four heuristics (`heuristics.hpp`): `zero` (equivalent to
  Dijkstra), `euclidean`, `manhattan`, and `haversine` (great-circle distance for OSM
  coordinates). Heuristic admissibility scales are derived from the data; a haversine
  scale defect was found and fixed (DISCREPANCIES §3–4).
* **Result record** (`shortest_path_result.hpp`): distance, path, and operation counters.

### 5.3 Classical priority queues (`cpp/include/lpq/pq/`)

| queue | file | notes |
|---|---|---|
| Binary heap | `binary_heap.hpp` | array-backed, position map for decrease-key |
| 4-ary heap | `binary_heap.hpp` (`FourAryHeap`) | same template, arity 4 |
| Pairing heap | `pairing_heap.hpp` | explicit node pool, decrease-key |
| Fibonacci heap | `fibonacci_heap.hpp` | amortised bounds; constant factors benchmarked |
| Radix heap | `radix_heap.hpp` | monotone integer keys only; the benchmark marks incompatible configurations unsupported |

Shared interface and comparison counting: `priority_queue.hpp` (`ComparisonCounter`,
`KeyLess`). [verified here: binary, 4-ary, pairing and Fibonacci are the typed queue tests in
`cpp/tests/pq/test_priority_queues.cpp`; radix has its own file, `test_radix_heap.cpp`.]

**v2 [PR #1]** adds a ring-sized bitmap bucket substrate, a cache-resident `PooledBucketQueue`
(with widths 1, 2, 4, 8, 16), and a classical `bucket_queue`. These are the queues that
actually produce the v2 speed result (§7).

### 5.4 The learned priority queue (`cpp/include/lpq/lpq/`, `cpp/include/lpq/verification/`)

| component | file | role |
|---|---|---|
| `LearnedPriorityQueue` | `learned_priority_queue.hpp` | assembles the components; the queue the search sees |
| `BucketSubstrate` | `bucket_substrate.hpp` | key-bucketed storage with overflow list and a cached verification bound |
| `CandidateManager` | `candidate_manager.hpp` | builds the candidate window from scores; enforces a maximum window |
| `VerificationEngine` | `verification/verification_engine.hpp` | checks an extracted candidate against the lower bound (exact) or within (1+ε) (approximate) |
| `CorrectionEngine` | `verification/correction_engine.hpp` | verify → expand → retry → classical fallback loop; every correction is recorded |
| `CorrectnessPolicy` | `verification/correctness_policy.hpp` | exact vs approximate policy; ε and budgets |
| `ApproximationReport` | `verification/approximation_report.hpp` | ratio summary: max, p95, p99, mean, within-ε fraction |
| `ExtractionObserver` | `lpq/extraction_observer.hpp` | hook for tracing and metrics without changing extraction |

Per the design, the learned scorer proposes ordering and the verifier decides. The
authoritative state is never mutated in a way that makes correctness impossible to audit.

### 5.5 Models, features and adaptation

**Predictors** (`cpp/include/lpq/models/`): a heuristic baseline, a linear predictor, a
logistic predictor (the deployed static scorer), a small MLP, a tree ensemble, calibration
(temperature), serialisation, and training-data structures. [verified here: the files
exist; their unit tests pass in the 847.]

**Honest status of the models** [doc: `docs/atlas/README.md` limitations; findings §4]:

* The **deployed model** is a logistic regression over six static node features, trained on
  labels from the *trusted reference* search (never from the learned queue itself).
* The **MLP and tree-ensemble** predictors are implemented and tested for correctness but
  have **no trained artifacts** and have not been evaluated against the task.
* The **feature set** is rich (28 features in the vector) but only six are used by the
  deployed scorer. Dynamic queue and search-state features are designed and listed in the
  PRD but were not built into the optimised path.

**Feature extraction** (`cpp/include/lpq/features/`): fixed-size `FeatureVector`, graph
features precomputed once, search-state features updated during the search. The design
forbids graph-wide scans in extraction.

**Adaptation** (`cpp/include/lpq/adaptation/`):

* `AdaptiveController` chooses a queue mode from measured gain, with an upper and a lower
  threshold (hysteresis) and a dwell time (minimum extractions in a mode) so it cannot
  thrash [verified here: the thresholds and dwell are in the header].
* `EWMA` utilities for smoothed statistics.
* `OnlineLearner` and `PredictionEvent` — the online-update path.
  **Important:** in both `main` and PR #1 the learner's `maybe_update` and `flush` have **no
  call site outside tests** [PR #1 `docs/experiments/v2-results.md` §3]. The adaptive
  configuration records events but holds no model, and no model weight changes in any
  benchmark or API run. Online learning is therefore **not active** in the measured system.

### 5.6 Metrics and instrumentation (`cpp/include/lpq/metrics/`, `cpp/include/lpq/trace/`)

* **Counters** (ESD §19): queue pushes, pops, decrease-keys, comparisons; LPQ predictions,
  candidate checks, corrections, fallbacks; feature/inference/verification time.
* **Phase profiler** (`phase_profiler.hpp`): attributes queue time to phases and reports its
  own overhead. Used by `lpq_profile`, which is a *separate binary* so probes never enter the
  timed benchmark.
* **Benchmark harness** (`benchmark_harness.hpp`): warm-up and measured repetitions, median
  and median absolute deviation (MAD), CPU pinning (`cpu_affinity.hpp`), `--require-pinning`.
* **Hardware info** (`hardware_info.hpp`): CPU model, cores, OS, compiler, flags, git
  commit, recorded with every result.
* **Hardware counters: not available on the development host** (WSL2 exposes no CPU PMU).
  They are reported as *unavailable with the reason*, never estimated. [doc:
  `docs/experiments/profiling.md`; verified here: `scripts/profile/check_counters.sh` is the
  check]
* **Trace recorder** (`trace/trace_recorder.hpp`): records a JSONL trace of a *separate*
  instrumented run of the same query. It returns an uncapped policy so observation does not
  change extraction. Tests verify that traced and untraced runs produce identical results.
* **Statistics** (`metrics/statistics.hpp`): used with the Python statistics package below.

### 5.7 The apps (`cpp/apps/`)

| executable | purpose |
|---|---|
| `lpq_run` | one query, one configuration; writes result JSON and optional trace; the engine call the API makes |
| `lpq_benchmark` | paired, pinned benchmark over one graph and one query set; `--experiment` runs a whole manifest; `--mode approximate --epsilon` checks every query against the reference |
| `lpq_profile` | phase profiler for the LPQ (separate binary) |
| `lpq_collect` | collects results into the stored result schema |
| `lpq_convert` | converts DIMACS/SNAP/OSM-derived inputs to the binary graph format |
| `lpq_export` | exports a trained model to the engine's binary format |
| `lpq_model_info` | prints model and feature information for the Atlas Model page |

Queue names accepted by the benchmark: `binary_heap`, `four_ary_heap`, `pairing_heap`,
`fibonacci_heap`, `radix_heap`, `lpq`, `lpq_model` (static trained scorer; requires
`--model`), `lpq_adaptive`. On PR #1 add `lpq_optimized`, `bucket_queue` and the interleaved
`--compare` geometry. [verified here: the dispatch lists in `lpq_benchmark.cpp` and
`lpq_run.cpp`]

### 5.8 Build and test

* CMake presets: `debug` (ASan, UBSan, instrumentation, `-Werror`), `release` (`-O3`,
  portable, no `-march=native`), `release-native`, `release-noinstr` (no counters compiled in).
* [verified here] `cmake --preset release` then `cmake --build --preset release` builds 147
  targets with **0 compiler warnings** on GCC 13.3, and `ctest --preset release` runs
  **847 tests, all passing**, in about 8 seconds.
* Labels: `unit` (819), `differential` (24), `fuzz-smoke` (4).
* [doc] Earlier clean-clone runs used GCC 12/15 and Clang 16/21, with ASan + UBSan + LSan.

### 5.9 Fuzzing and defects found by it

Four libFuzzer targets (`cpp/fuzz/`): the binary graph reader, the model loader, the
experiment-manifest reader, and queue operation sequences. Two real defects were found this
way and are recorded in the discrepancy register [doc: DISCREPANCIES defects 1 and 6]:

1. **Bucket substrate returned keys out of order** beyond its window. In exact mode this
   returned a non-optimal distance (on NY: 1,154,372 returned vs 1,139,254 exact). Found by
   `fuzz_queue_ops` on its first run. Stored results were not affected, because every
   recorded run used a bucket width where no key overflowed.
2. **Binary graph reader trusted the header's counts.** A 72-byte file could request 5.2 GB.
   Fixed by checking declared counts against the remaining bytes before allocation.

Additional defects, found by tests or review: overflow re-filing discarded learned scores;
haversine A* used the Euclidean scale; DIMACS "geographic" coordinates were microdegrees
rather than degrees; the Python header reader lagged the graph format version.

---

## 6. The Python tooling (`python/lpq_tools/`)

| package | responsibility |
|---|---|
| `data/` | dataset acquisition (`acquire.py`, `sources.py`), validation (`validate.py`, `binary_header.py`), OSM road extraction with a documented access policy (`osm.py`), query generation (`queries.py`), and the end-to-end preparation pipeline (`pipeline.py`) |
| `training/` | training datasets from reference runs (`dataset.py`), training and calibration of the static scorer with a seed (`train.py`), model writer for the engine format (`model_io.py`), and the train/evaluation leakage guard (`leakage.py`) |
| `evaluation/` | ablation driver (`ablation.py`), graph-generalisation driver (`generalisation.py`), results loader and validator (`results.py`) |
| `statistics/` | paired comparison with permutation tests and Holm correction (`comparison.py`); summaries and bootstrap intervals (`summary.py`) |
| `plotting/` | figure generation with a shared style (`figures.py`, `style.py`) |
| `utilities/` | hashing, JSON I/O, repository paths |

Key properties, all from the code and tests [verified here: 222 Python tests pass]:

* **Seeds are required, never defaulted** in library code (`train.py` says so explicitly).
* **Leakage guard.** `leakage.py` checks that evaluation queries never overlap training
  queries where disjointness is required. The UI shows `Training/Evaluation split: DISJOINT`
  or a warning. This guard was added after a real leak: the first model was trained on the
  same queries it was then benchmarked on, which inflated the measured speedup by about a
  third (2.10 ms → 1.43 ms after the fix).
* **Training report** carries the seed, graph checksum and query seed. **Gap:** the saved
  model artifact (`write_linear_model`) stores weights, the feature-layout version and
  online hyperparameters, but does not embed a checksum, seed, training-query list or
  metrics. Those live in the report. See the status document.
* **PR #1 adds** a queue-policy trainer (`lpq_tools/policy/`) with nested
  leave-one-topology-out evaluation, and a controlled reweighting tool that makes 12 derived
  graphs (same topology, synthetic weights).

---

## 7. Datasets

| id | family | nodes | arcs | status in this checkout |
|---|---|---:|---:|---|
| `dimacs_ny` | DIMACS road (New York) | 264,346 | 733,846 | metadata tracked; graph not tracked (must be acquired) |
| `dimacs_col` | DIMACS road (Colorado) | 435,666 | 1,057,066 | same |
| `dimacs_fla` | DIMACS road (Florida) | 1,070,376 | 2,712,798 | same |
| `snap_roadnet_pa` | SNAP road (Pennsylvania) | 1,088,092 | — | same |
| `snap_roadnet_ca` | SNAP road (California) | 1,965,206 | — | same |
| `snap_facebook` | SNAP social | ~4k | — | same |
| `osm_monaco` | OSM drivable network | 5,692 | 8,495 | **pinned Geofabrik URL returned HTTP 404 in CI on 2026-10-11** [PR #1 config comment] |
| `osm_liechtenstein` | OSM drivable network | 29,945 | 59,358 | same snapshot date; same risk (not separately verified) |
| `synth_ratio1_ny` | synthetic | 264,346 | — | NY topology, synthesised weights; constructed to test a hypothesis |
| `derived_*` (PR #1) | synthetic weights on real topologies | — | — | 12 graphs, labelled as derived, Dijkstra only |
| `demo_road` (PR #1) | bundled offline demo | small | — | committed so setup and CI run without network |

Node and arc counts above are from `datasets/metadata/index.json` and the per-dataset
metadata files [verified here for the OSM and the DIMACS entries; others from the docs].

OSM policy [doc: `docs/atlas/README.md`]: drivable `highway` classes; `access` and
`motor_vehicle` exclusions; one-way handling; bridges and tunnels via shared nodes only;
largest strongly connected component; **no turn restrictions**; weights are great-circle
decimetres, so a route is the *shortest*, never the *fastest*.

---

## 8. Experiments and what has been measured

Experiments are configured by manifests (`configs/experiments/*.yaml`, query manifests in
`experiments/manifests/`). The reference matrix is `dimacs_ny_exact_v1.yaml`: six queues,
exact mode, 100 queries, 11 measured repetitions after 3 warm-up, seed 42, all in one pinned
process so comparisons are paired.

Measurement rules the project applies [doc: PRD §18, §22; ESD §19–25; README §10]:

* One pinned core for timed runs; median and MAD reported; outliers are never silently
dropped.
* Paired comparisons on identical query sets, with paired permutation tests and Holm
correction; effect sizes and fraction of queries reported beside p-values.
* **Comparison counts are exact and reproducible; wall-clock is not.** Only configurations
run inside one invocation may be compared on time. Identical work measured across four
invocations varied by a factor of 2.3 in median time.
* Every correctness check compares against the trusted reference, and benchmarks abort on
any mismatch.

### 8.1 Main results on `main` (v1) [doc: `experiments/paper/findings.md`]

Comparison counts on `dimacs_ny`, 100 queries, exact mode:

| rung | comparisons | vs previous |
|---|---:|---:|
| binary heap | 203,801,439 | 1.000× |
| radix heap | 71,839,857 | 0.352× |
| LPQ (window + verification) | 177,580,603 | 2.472× |
| LPQ + trained static scorer | 177,732,303 | 1.001× |

Read as a ladder: bucketing alone removes 2.8× of the comparisons, and the candidate
window and verification give 2.5× of that back. Wall-clock: the window and verification
cost about 24.5 ms per query and were slower on every query measured. Verification plus
window maintenance are about 76% of extraction time.

Generalisation across six graphs: the learned queue's isolated cost ranges from 1.001×
(`snap_roadnet_pa`) to 4282× (`snap_roadnet_ca`). The explanation the project first offered
(bucket width) was wrong; the surviving variable is bucket width divided by mean edge
weight, which predicted a constructed test case to within 1.28×.

Learned scorer: held-out ROC AUC **0.79** on `snap_facebook` (22× precision lift over
chance; pre-correction protocol, 0.80 and 23× under the disjoint protocol, see D-19) but **0.53** on `dimacs_ny` (1.12×). Road networks have near-uniform degree, so
static degree features carry little signal there.

Approximate mode [doc: `docs/experiments/approximate-mode.md`]: on `dimacs_ny`, ε = 0.1
removed 44% of comparisons at default settings, and approximation made the queue robust to a
badly chosen bucket width. **This was measured on 5 queries**, and the document itself says so.
ε saturates by 0.01 on `dimacs_ny`.

A* [doc: `docs/experiments/astar.md`]: removes 54% of comparisons from the classical queues
and 33% from the LPQ, so the LPQ's relative cost rises to 3.632× under A*. Euclidean only;
`manhattan` and `haversine` were not run.

**Historical, retracted: the v1 claim that the adaptive controller cuts comparisons by 34%.**
v1 recorded it as the one place the learned machinery beat its own baseline (116,608,480 vs
177,580,603 comparisons at identical pop counts). The v2 audit reports that the adaptive
configuration's count is within 0.05% of the plain classical bucket queue's (116,552,995),
so the reduction came from retreating to a classical scan, not from learning [PR #1
`v2-results.md` §3; the two counts are **[doc]**, not re-run here]. **v2 does not validate the
claim; it contradicts the attribution.** The original statement remains in
`experiments/paper/findings.md`, `docs/atlas/ABSTRACT.md`,
`docs/experiments/adaptive-controller.md` and `docs/experiments/README.md` on `main`, on the
PR #1 branch and on the docs branch; v2 does not edit those files. On `completion/v2-baseline`
each now carries a dated retraction notice with the original text left in place.

### 8.2 v2 results on PR #1 [PR #1 `docs/experiments/v2-results.md`]

Hardware: a cloud VM (Intel Xeon 2.8 GHz, 4 vCPU, KVM, GCC 13.3, `-O3`). No hardware
counters. Interleaved, paired, one pinned core, 7 repetitions, all answers exact:

| workload | queries | radix median | `lpq_optimized` vs radix (95% CI) |
|---|---:|---:|---|
| NY, Dijkstra | 100 | 12.66 ms | **0.916** [0.892, 0.939] — faster on 79% of queries |
| NY, A\* | 100 | 6.15 ms | 0.977 [0.946, 1.009] — resolves to radix |
| Facebook, Dijkstra | 100 | 0.35 ms | 1.008 [0.983, 1.034] — resolves to radix |
| COL, Dijkstra | 30 | 31.77 ms | 1.050 [0.980, 1.125] — resolves to radix |

Findings from the v2 work:

* **The gain comes from queue engineering plus a per-graph calibrated choice, not from a
  learned predictor.** `lpq_optimized` calibrates radix and pooled bucket queues on 12
  sampled queries per graph and machine, and picks a pooled queue only if it beats radix by
  at least 3% and wins on at least 60% of pairs. The decision is cached per graph, CPU and
  settings. Calibration cost on NY was 2.7 s once.
* **The original LPQ design is 3.10× slower than radix on NY** with a constant scorer and
  3.17× with the trained scorer. In-place "direct" extraction reduces it to 1.46×; that is
  queue engineering. The trained scorer gives no gain (1.46× → 1.66× with a front of 32,
  because maintaining the score-ordered front costs more than the hint saves).
* **Replay floor:** replaying a recorded pop sequence with queue work removed takes 0.47–0.51×
  the radix time on the DIMACS road graphs. So at most about half of a radix search is queue
  time there, and no queue, learned or not, can save more than roughly half on those graphs.
* **Learned queue-policy:** a ridge policy over weight statistics was evaluated with nested
  leave-one-topology-out. It **never beat radix** (0 of 17 held-out graphs). It is not the
  default. This is a negative result, kept.
* **Retractions (recorded in `v2-results.md` §3 and the PR description only):** the 34%
  adaptive claim (§8.1) is retracted, and the v1 wording that put the online learner inside
  the measured system is not supported: `OnlineLearner::maybe_update` and `flush` have no call
  site outside tests (§5.5; confirmed by `git grep` here).

PR #1's own limits section: one graph with a verified gain; A\* not improved; Windows scripts
verified only under PowerShell 7 on Linux; the old Monaco OSM download fails in the sandbox.

---

## 9. LPQ Atlas: the interactive system

### 9.1 Architecture [doc: `docs/atlas/README.md`, `specdoc.md` §19–25]

```text
atlas/web  (Next.js, TypeScript, Leaflet)
   │  REST + server-sent events (same-origin /api rewrite)
atlas/api  (FastAPI) — validation, dataset/model registry, file-backed jobs, trace streaming
   │  argv lists built from enums (no shell)            │ imports
cpp/apps/lpq_run, lpq_benchmark, lpq_export, lpq_model_info   python/lpq_tools
   │
datasets/ · models/ · results/raw · results/atlas (job directories)
```

* **No shell anywhere.** Every engine call is an argument list built from enums and integers.
  Dataset, model and manifest identifiers are regex-validated and resolved against the
  registry's directory listing, never joined into paths.
* **Persistence is filesystem-only.** Each run, comparison or benchmark is a directory with
  `request.json`, `status.json`, the engine's `result.json` / `trace.jsonl` / `results.json`,
  and `engine.log`. Job states: `QUEUED → PREPARING → RUNNING → FINALIZING → COMPLETED |
  FAILED | CANCELLED`.
* **Scale modes:** `teaching` (≤ 20k nodes, full visual trace), `route` (≤ 400k nodes with
  coordinates, capped trace, cap reported), `research` (no coordinates or larger; metrics and
  node ids, no map).
* **Observability:** structured JSON logging (`logging_setup.py`).
* **Engine seam:** `dijkstra()` and `astar()` take an optional observer template; the default
  null observer compiles every hook out, so benchmarked code is unchanged.

### 9.2 Pages (`atlas/web/app/`)

`/` overview · `/explore` route explorer (click a source and destination, run Dijkstra or
A*, replay the real trace, see metrics) · `/compare` synchronised comparison of two
configurations on the same query, with a reference check · `/benchmarks` experiment
configuration and results · `/models` and `/models/[id]` model registry and inspector ·
`/datasets` and `/datasets/[id]` dataset registry and details · `/research` aggregate stored
results · `/learn` architecture explanation · `/runs/[id]` persistent run view · not-found page.
PR #1 adds a **Quick Compare** landing page (vanilla and optimised Dijkstra and A\*, radix
baselines, shuffled multi-round measurement, majority-of-rounds verdicts, resolved-queue
reporting).

### 9.3 Honest labelling in the UI [doc: `docs/atlas/README.md` "What is real"]

| on screen | source |
|---|---|
| distance, path, counters, LPQ statistics | `lpq_run` result JSON, unmodified |
| benchmark runtime | untraced repetitions inside `lpq_run` |
| animated exploration | JSONL trace of a *separate* instrumented run of the same query |
| "trace reproduces the measured run" | `lpq_run` compares traced and untraced runs |
| exact / ratio badge | checked against the reference in the same process |
| hardware counters | shown as **unavailable** with the reason |

### 9.4 Running it [doc + verified here]

```bash
# Engine (verified here: configure, build, ctest all succeed)
cmake --preset release && cmake --build --preset release
ctest --preset release

# Python (verified here: 222 passed, 8 skipped)
python -m pip install -r python/requirements-dev.txt
cd python && python -m pytest -q

# API (verified here: 26 passed)
python -m pip install -r atlas/api/requirements.txt
cd atlas/api && python -m pytest -q

# Web (verified here: install, lint with 0 errors and 7 warnings, unit tests 5/5, production build)
cd atlas/web && npm ci && npm run lint && npm test && npm run build

# Whole stack
docker compose up --build        # then open http://localhost:3000
```

Benchmarks: `./build/release/bin/lpq_benchmark --experiment configs/experiments/dimacs_ny_exact_v1.yaml --output results/raw/dimacs_ny_exact_v1.json`
(needs the converted NY graph, which must be acquired first).

PR #1 adds `scripts/setup/{doctor,setup,run,test}` (bash and PowerShell) and
`scripts/demo/reproduce_benchmark.sh`, which run without network access.

---

## 10. Quality, CI and reproducibility

| item | state |
|---|---|
| Clean configure and build of the engine | **verified here** on `main` (147 targets, 0 warnings) |
| C++ tests | **verified here**: `main` 847/847; PR #1 914/914, 0 warnings (PR #1 checked in a separate worktree) |
| Python tests | **verified here**: `main` 222 passed, 8 skipped; PR #1 300 passed, 12 skipped (the PR text claims 311 passed) |
| API contract tests | **verified here**: `main` 26/26; PR #1 87 passed, 1 skipped (the PR text claims 77) |
| Web lint, tests, build | **verified here**: `main` lint 0 errors (7 warnings), unit tests 5/5, build succeeds; v2 head (M1) `npm ci`, lint (0 errors, 7 warnings), typecheck, unit tests 41/41 and build all exit 0 |
| End-to-end (Playwright) | **verified here** on the v2 head, `demo_road` graph: 8 passed, 2 skipped (unreachable-destination flow and research/model pages skip for missing data). **Real OSM is untested** |
| v2 head, other suites (M1) | **verified here**: C++ 914/914, Python 300 passed + 12 skipped (all for missing data), API 87 passed + 1 skipped. Single executions. |
| Hosted CI on `main` | no run on the `main` branch itself was among the 8 most recent runs inspected; **untested**. The workflow triggers on every push |
| Hosted CI on PR #1 | [run #27](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38148343456), `e6dde1c`, 2026-10-11: **all 5 jobs green** (`engine`, `web`, `fuzz`, `windows-scripts`, `e2e`). Its `e2e` job runs on the bundled `demo_road`, not on real OSM [GitHub Actions, read via API] |
| Hosted CI on the docs commit | [run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184), `b2049c2`, 2026-10-13: `engine`, `web`, `fuzz` succeeded; **`e2e` failed** at the OSM Monaco preparation step with `HTTP 404 Not Found` [GitHub Actions, job log read via API] |
| Fuzz | 4 libFuzzer targets; bounded CI job green; long campaigns not run |
| Sanitisers | ASan/UBSan debug preset defined; [doc] clean ASan/UBSan/LSan runs on Clang recorded earlier |
| Hardware counters | **unavailable** on every host used; documented, never estimated |
| Reproduction scripts | `scripts/reproduce/{build_all,run_main_experiments,generate_tables,generate_figures}.sh`; each stops at the first thing it cannot verify |
| Pinned seeds | query seed 42; training and statistics seed 20260923 [doc: `docs/reproducing.md`] |

**Reproducibility limits** that a reader must know:

* Raw graphs, processed graphs, trained models and `results/raw` are **not in the repository**.
  Every reproduction starts with dataset acquisition, which is a separate, network-dependent
  step with checksums.
* The **OSM pinned snapshot is unreachable**: the Monaco extract returned HTTP 404 in CI on
  2026-10-11, and the Liechtenstein extract uses the same dated snapshot. The OSM acquisition
  step therefore cannot be reproduced from the pinned URL today. The CI e2e job uses the
  bundled `demo_road` instead.
* Comparison counts reproduce to the unit on any machine. Timings do not, and are not claimed
  to.

---

## 11. Known limitations (what the project does not show)

1. **The learned component does not improve end-to-end runtime** on the workloads measured,
   on any graph except the one v2 result, which comes from queue engineering rather than
   learning.
2. **Online learning is not active** in the measured system. The learner exists and is tested
   in isolation; no benchmark or API run changes model weights.
3. **Five road networks, one social graph, one constructed graph, one OSM-scale claim.** No
   other graph class in the measurements. Road closures, additions and other modifications
   are not tested.
4. **Bucket width was not swept within one natural graph**; each dataset contributes the one
   width the runner derives. The sweep on `synth_ratio1_ny` begins it.
5. **Approximate mode was measured on 5 queries** on the main road network and one random
   graph.
6. **A\* was measured on one heuristic and one graph** on `main`.
7. **No hardware counters.** Cache, branch and LLC behaviour is not measured. The hypothesis
   that the substrate's memory behaviour explains the per-extraction cost is untested.
8. **MLP and tree predictors** have no trained artifacts and have not been evaluated.
9. **Dynamic features** (queue and search-state features) are not in the optimised path.
10. **Comparison count is not runtime.** The project measured a queue doing 12.9% fewer
    comparisons than a binary heap while running about 4.6× slower.
11. **CI is red on the v1 workflow and green on the v2 workflow for different reasons.** The
    v1 workflow (on `main` and the docs branch) fails at the OSM download; the v2 workflow
    avoids the download by using `demo_road`, so its green result says nothing about real OSM.
    No hosted run on the `main` branch itself was inspected.
12. **OSM acquisition is currently broken** for the pinned Monaco snapshot (CI `HTTP 404`
    on 2026-10-11 and again on 2026-10-13). Liechtenstein uses the same snapshot date and is
    untested.
13. **Turn restrictions are not modelled** in OSM graphs; weights are distances, not travel
    times.

---

## 12. The specifications this project is held to

The repository's rules come from these documents. They are authoritative in this order for
the research engine (PRD, ESD, README, CLAUDE.md) and for the Atlas platform (`specdoc.md`).

| document | role |
|---|---|
| `README.md` | how to start; milestone sequence; success checklist (§19) |
| `PRD.md` | what must be achieved; acceptance criteria (§25); success definition (§29) |
| `ESD.md` | how to engineer it; implementation order (§39); Definition of Done (§40) |
| `CLAUDE.md` | standing rules: correctness before optimisation, no fabrication, seeds, pinning, sanitisers, hardware counters, Graphify use |
| `specdoc.md` | LPQ Atlas completion specification: truth reconciliation, OSM, backend, frontend, research integrity, Definition of Done (§51–55), phases (§56), acceptance scenario (§59) |

Two standing rules from `CLAUDE.md` shape every claim in this project:

* **Never fabricate.** Do not report benchmark numbers, accuracy, approximation ratios, or
  completed functionality that was not measured or implemented. An unmeasured claim must be
  labelled unmeasured.
* **Negative results stay visible.** `specdoc.md` §32 and §55 require that the findings which
  do not favour the learned queue remain visible, and that unsupported metrics are marked
  unavailable.

---

## 13. Glossary

| term | meaning |
|---|---|
| **SSSP / single-pair** | shortest path from one source (to one target) |
| **CSR** | compressed sparse row graph layout (offsets, edges, weights arrays) |
| **Priority queue (PQ)** | structure returning the minimum-key item; here, the frontier of a search |
| **Decrease-key** | lowering a queued item's key when a shorter path is found |
| **Relaxation** | examining an edge and possibly improving a neighbour's distance |
| **Radix heap** | monotone integer-key queue using buckets by bit position; fast when keys are integers and monotone |
| **Bucket substrate** | key-range buckets with a window and an overflow list |
| **LPQ** | learned priority queue: a queue whose extraction is steered by a learned score and checked by verification |
| **Candidate window** | the top-scored entries considered for extraction |
| **Verification** | the check that an extracted candidate is valid (exact lower-bound test, or (1+ε) test in approximate mode) |
| **Correction / fallback** | widening the window, then classical scanning, when verification fails |
| **ε (epsilon)** | the approximation slack; approximate result within (1+ε) of optimal |
| **Static scorer** | a model evaluated once per node before the search, on static features |
| **Online learning** | updating model weights from verified outcomes during a run (not active in the measured system) |
| **Replay floor** | the time a search would take with queue work removed; bounds what any queue can save |
| **Paired / interleaved** | every configuration run on the same query, back to back, in shuffled order |
| **MAD** | median absolute deviation, the spread measure reported with medians |
| **Holm correction** | multiple-comparison adjustment for the paired tests |
| **PMU** | the CPU's hardware performance monitoring unit; absent under WSL2 |
