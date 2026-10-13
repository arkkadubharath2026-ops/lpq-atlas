# LPQ Atlas — Completion Status and Remaining Work

**Question answered:** is the project 100% finished?
**Answer:** **No.** Measured against the project's own acceptance criteria, the research
system is about **70%** complete and the Atlas platform is about **93%** complete. The
binding definition of "technically complete" is PRD §25, and it is not met: online adaptation
does not run, the OSM acquisition step is broken, and several required experiments and
ablations do not exist yet. Section 6 lists what is left, in order.

Companion document: [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) (what the project is).
Progress log: [IMPLEMENTATION_PROGRESS.md](IMPLEMENTATION_PROGRESS.md).

> **Revision note (M2, 2026-10-13, branch `completion/v2-baseline`).** This document was first
> written on 2026-10-13 from the `main` checkout. The audit in milestones M0 and M1 found
> errors in it. They are corrected in place below, and listed here so the change is visible:
>
> 1. **The 34% adaptive-controller claim.** The first version said PR #1's documents supersede
>    it and that it would be "corrected on merge". **That was wrong.** PR #1 does not edit
>    `findings.md`, `ABSTRACT.md`, `adaptive-controller.md` or `docs/experiments/README.md`;
>    all four still state the claim on `main`, on the PR #1 branch and on the docs branch. The
>    retraction exists only in `docs/experiments/v2-results.md` §3 and the PR #1 description.
>    On **this branch only**, M2 annotates those four files with a dated retraction notice and
>    leaves the original text in place. The claim is **historical and retracted**. Nothing in v2
>    validated it. The v2 audit reports the opposite (see M10).
> 2. **Dependency conflict removed as a repository defect.** The first version listed a
>    `starlette` / `sse-starlette` pin conflict in `atlas/api/requirements.txt`. No file in the
>    repository mentions `sse-starlette`. In the audit sandbox it is required by the `mcp`
>    package, which is not a project dependency. See M16.
> 3. **CI facts updated.** Hosted CI has run (see M19). Run #28 on the docs commit failed at the
>    OSM step, and v2's green run uses the bundled `demo_road`, not real OSM.
> 4. **v2 test baseline added.** The M1 baseline ran the v2 head in a clean worktree. Counts
>    are in §0.1 and §7.
> 5. **Script modes made precise.** 20 files remain at mode 100644 on v2: 8 `.sh`, 7 `.ps1`,
>    5 `.py`. Only the `.sh` files that the docs run directly need the executable bit (M16).
> 6. **Percentages were not re-scored.** The criteria and evidence behind the milestone scores
>    did not change, so no score changed. The corrections above change wording and facts, not
>    the matrices.
>
> **M3 update (2026-10-13, branch `completion/m3-osm-acquisition`).** `osm_monaco` was re-sourced
> from the openstreetmap.fr extract and verified; details are in `DISCREPANCIES.md` D-01 and
> `IMPLEMENTATION_PROGRESS.md`. The statements below about a broken OSM acquisition describe
> **Monaco as of M2** and are superseded for Monaco only; **Liechtenstein is still unavailable**,
> and the Monaco pin holds only for the retained bytes (the publisher replaces the file daily).
> The scoring matrices and milestone percentages (§2, §3) are **as of M2 and were not re-scored in
> M3**, so rows that cite the broken OSM step (for example the §54 "reproducible PBF acquisition:
> 0" cell) have not been updated; the owner decides whether to re-score.

---

## 0. How to read the percentages

* **Milestones (§2)** are the work units. Each has a percentage: the share of its own
  definition that is implemented *and* evidenced. A percentage is a judgement against stated
  criteria, not a measurement. The criteria are written next to each milestone.
* **Acceptance matrices (§3)** score each criterion of the authoritative lists as **1** (met),
  **0.5** (partly met, with the gap named), or **0** (not met). A percentage is the sum over
  the list divided by its size.
* **Overall** is the equal-weight average of the 22 milestones (§2), which comes to 71%. The
  research-criteria figure (70%) and the platform figure (93%) are shown separately because
  they answer different questions. The research figure is the one that governs "finished".
* **Evidence labels.** **[verified here]** means I ran it in the audit sandbox on 2026-10-13
  (the first-session runs on `main` and the M1 baseline on the v2 head). **[CI]** means a
  hosted GitHub Actions job I read through the API. **[doc]** means it is stated in a project
  document and was not re-run. **[PR #1]** means it is on the open draft PR, not on `main`.
  **[untested]** means the check was skipped or not run, with the reason given.

### 0.1 The two states of the repository

| state | what it is | verified here |
|---|---|---|
| `main` @ `4e7ad00` (= local checkout `claude/sleepy-heisenberg-fhr3me`) | v1: original engine, first measurements, LPQ Atlas v1 | build 147 targets, 0 warnings; ctest 847/847; pytest 222 passed, 8 skipped; API 26/26; web lint 0 errors / 7 warnings, tests 5/5, build OK |
| PR #1 branch `claude/affectionate-gates-53lchk` @ `e6dde1c` (21 commits ahead, **open draft, not merged**) | v2: autotuned exact queue, direct extraction, policy trainer, Quick Compare, local setup, and `v2-results.md` (which records the 34% retraction) | **M1 baseline, clean worktree:** configure and build exit 0, 155 targets, 0 warnings; ctest 914/914 (unit 886, differential 24, fuzz-smoke 4); pytest 300 passed, 12 skipped; API 87 passed, 1 skipped; web `npm ci`, lint (0 errors, 7 warnings), typecheck, unit tests 41/41, build all exit 0; Playwright e2e 8 passed, 2 skipped. Hosted CI run #27 on this commit: 5/5 jobs green [CI] |
| `completion/v2-baseline` (local only, no upstream) | `e6dde1c` plus the docs commit (cherry-pick of `b2049c2`) plus the M2 documentation corrections | documentation only; no code, dependency, workflow or benchmark change relative to `e6dde1c` |

**What is and is not verified for v2.**
* The counts above were produced by the M1 run on `e6dde1c`. They are single executions, so
  flakiness is not ruled out.
* The 12 Python skips are all missing-data skips: `snap_facebook` not prepared (7 tests in
  `test_training_dataset.py`), the Monaco snapshot not acquired (`test_osm.py`), the
  `dimacs_ny` graph or profile missing (`test_policy_core.py`, `test_policy_export.py`), and a
  v2 result file not present. So the **training and leakage pipeline, OSM extraction, and
  policy export are not exercised against real data** by this baseline. **[untested]**
* The 2 skipped Playwright tests are the unreachable-destination flow (needs `snap_roadnet_pa`)
  and the research/model pages (need stored NY results and a NY model). The unreachable-route
  flow (specdoc §43 item 10) is therefore **[untested]** at the UI level in this baseline.
* The 1 skipped API test is skipped by design (the failure path cannot be provoked once
  `lpq_optimized` exists).
* Not run in M1: the libFuzzer job, the Debug ASan/UBSan preset, `ctest --preset release-fast`,
  any Windows build, any benchmark, the reproduction scripts. **[untested]**

PR #1's own text claims Python **311** passed and API **77** passed. The M1 run found Python
**300 passed + 12 skipped** and API **87 passed + 1 skipped**. The pass/fail result agrees, the
reported counts do not, and the PR text should be corrected. (Not edited in M2: the PR is out
of scope. A proposed replacement is in `IMPLEMENTATION_PROGRESS.md`.) The PR's C++ count (914)
and web unit count (41) match.

**Where the two states disagree, this document follows the code and CI evidence, not PR #1's
prose.** In particular, v2 does **not** correct the v1 documents that state the 34% adaptive
claim (see the revision note and M10). The v2 audit's retraction lives in `v2-results.md` §3
and the PR description only.

---

## 1. Verdict by question

| question | answer | basis |
|---|---|---|
| Does the engine build and pass its tests? | **Yes** | [verified here] both states; [CI] PR #1 |
| Is LPQ exact mode correct? | **Yes** — every benchmark and API result is checked against the reference Dijkstra and aborts on mismatch | [verified here] tests; [doc] benchmark runner |
| Does the learned component make the queue faster end to end? | **Not on the measured workloads**, except one NY Dijkstra result where the speedup is from queue engineering and a calibrated queue choice, not from learning | [doc] v1 findings; [PR #1] v2-results §4–§8 |
| Does online learning run? | **No.** `OnlineLearner::maybe_update` and `flush` have no call site outside tests | [verified here] `git grep` on both the `main`-derived and v2 trees: defined at `online_learner.hpp:246` and `:259`, referenced only by `test_online_learner.cpp`; [PR #1] `v2-results.md` §3 agrees |
| Is the OSM pipeline usable from the pinned sources? | **Monaco: yes, with a durability limit (M3).** The old Geofabrik snapshot returned HTTP 404 in CI on 2026-10-11 and 2026-10-13; Monaco was re-sourced from the openstreetmap.fr extract and pinned by SHA-256, which holds only for the retained bytes because the publisher replaces the file daily. **Liechtenstein: no** (still names the dead URL) | [CI] run #28, `e2e` step 7, `HTTP 404 Not Found`; [verified here, M3] pipeline run, 1000-query correctness checks and a real-OSM Playwright run (8 passed, 2 skipped) |
| Is the Atlas platform usable? | **Yes** on the bundled `demo_road` graph, with the OSM caveat and 7 lint warnings. Real-OSM operation is **[untested]** | [verified here] M1 e2e 8 passed, 2 skipped; [CI] run #27 |

---

## 2. Milestones

Overall average: **71%** over 22 milestones.

### M0 — Truth reconciliation and inventory — **90%**
* **Criteria:** repository inventory; authoritative modules identified; documentation claims
  mapped to code; builds and tests confirmed; no reliance on generated build state; schemas frozen.
* **Done:** a full inventory was made from the source (313 tracked files on `main`); the
  build and test suites were run from clean configure in both states; the discrepancy register
  exists (`docs/atlas/DISCREPANCIES.md`); result and trace schemas are versioned.
* **Remaining:** the discrepancy register was stale: D-06 said "not yet run on GitHub", but
  hosted runs exist, and the register did not record the v2 audit findings (online learning,
  the 34% attribution). **Addressed in M2 on this branch only** (`DISCREPANCIES.md` entries
  D-01, D-02, D-06 updated and D-11 to D-16 added). The same files on `main`, the docs branch
  and the PR #1 branch are unchanged and still stale.

### M1 — Build system and clean build — **95%**
* **Criteria:** CMake presets; clean build on Linux and Windows; C++20 compiler selection
  enforced; pinned generator.
* **Done:** [verified here] `cmake --preset release` configures and builds on GCC 13.3 with
  0 warnings. The presets (`debug`, `release`, `release-native`, `release-noinstr`) exist.
  [doc] earlier clean-clone builds passed on GCC 12/15 and Clang 16/21.
* **Remaining:** no Windows **engine** build runs in CI. The `windows-scripts` job only parses
  PowerShell and runs the read-only doctor. D-08 records that the Windows build fails under a
  deep source path. A native Windows build is therefore not verified.

### M2 — Graph representation, I/O and input validation — **95%**
* **Criteria:** CSR graph; directed and undirected; SNAP and DIMACS readers; checksummed binary
  format; malformed input rejected; metadata.
* **Done:** [verified here] CSR, graph I/O, metadata and fuzz targets are in the 914/914 suite.
  Binary format v3 stores the coordinate system (a v1 defect is fixed, with regression tests).
  The binary reader rejects headers that claim more data than the file holds (found by libFuzzer).
* **Remaining:** a corpus-based fuzz campaign longer than the 60 s per target that CI runs.

### M3 — Reference Dijkstra and classical priority queues — **100%**
* **Criteria:** trusted reference; binary, pairing, Fibonacci, radix heaps; differential tests
  against the reference; radix marked unsupported where its preconditions fail.
* **Done:** [verified here] 847/847 (`main`) and 914/914 (PR #1) pass, including 24
  differential tests. The reference linear-scan Dijkstra agrees with the std-heap oracle on all
  distances. The 4-ary heap is included. Radix is listed as unsupported for non-monotone keys.
* **Remaining:** none that the acceptance criteria require.

### M4 — Benchmark infrastructure and statistics — **90%**
* **Criteria:** pinned single-core timing; median and MAD; warm-up separated; paired runs;
  manifests; machine-readable output with hardware metadata; no outlier dropping; interleaved
  option.
* **Done:** [doc] the harness, `--require-pinning`, median/MAD, the manifest runner
  (`--experiment`), and hardware metadata in every record. PR #1 adds `--interleave` (seeded
  per-query shuffle) and `--compare` geometry. The paired permutation test with Holm correction
  and the bootstrap summary are in `python/lpq_tools/statistics/`.
* **Remaining:** verify the hardware metadata is present in every stored record type
  (`lpq_collect`). This check was not run here. Also resolve the cross-invocation timing
  problem: results from separate invocations must not be compared on time (documented, not
  eliminated).

### M5 — A* and heuristics — **80%**
* **Criteria:** A* with zero, Euclidean, Manhattan and haversine heuristics; admissibility;
  A* on OSM.
* **Done:** [verified here] A* with all heuristics is implemented and its tests are in the
  suite. [doc] a haversine scale defect and a DIMACS microdegree defect were found and fixed.
  A* measured on NY (Euclidean) with every queue. [PR #1] A* on NY, FB and COL.
* **Remaining:** `manhattan` and `haversine` were not benchmarked on any graph (v1 doc §6).
  A* does not improve under the v2 queue (NY 0.977×, CI includes 1). The OSM A* result depends
  on the broken OSM acquisition (M12).

### M6 — Instrumentation, tracing and profiling — **75%**
* **Criteria:** compile-time-switchable counters and timers; zero-cost-disabled path; phase
  profiler separate from timed runs; trace recorder that does not change extraction; traced
  and untraced runs agree.
* **Done:** [verified here] the counters, the phase profiler (`lpq_profile`, separate binary),
  and the trace recorder are implemented. `test_trace` checks that observed and unobserved runs
  agree. [doc] the phase profile is recorded (verification 35.1%, window maintenance 30.0%).
* **Remaining:** the profiler's own overhead is 1.40× and three phases are below its resolution.
  These are reported, not fixed. The counters for cache, branch and LLC are not collectable on
  this host (M15).

### M7 — Features, predictors and training pipeline — **70%**
* **Criteria:** feature vector; graph features precomputed; predictors (heuristic, linear,
  logistic, MLP, tree ensemble); calibration; serialisation; training separated from
  evaluation; seeds explicit; leakage guard; model artifact metadata linking features, data,
  queries, hyperparameters, seed, metrics and checksum (specdoc §30).
* **Done:** [verified here] predictor, calibration, serialisation and feature tests are in the
  suite. The deployed static scorer (logistic, six features) is trained on reference-run labels
  with a required seed. The leakage guard is automated and shows `DISJOINT`. The training
  report records seed, graph checksum and query seed.
* **Gap 1 — artifact metadata:** `write_linear_model` embeds weights, a feature-layout version
  and online hyperparameters. It does not embed a checksum, seed, training-query list, or metrics.
  Those sit in the report, not in the artifact (specdoc §30 is not fully met).
* **Gap 2 — MLP and tree ensemble:** implemented and tested for correctness, with no trained
  artifacts and no evaluation on the task.
* **Gap 3 — feature set:** 28 features exist; only six are used.
* **Remaining:** the items above, and a decision recorded on whether the MLP and tree models
  are evaluated or dropped (PRD §10: the simplest model that wins).

### M8 — LPQ core: substrate, candidates, verification, correction (exact) — **90%**
* **Criteria:** bucket substrate; candidate manager with a maximum window; verification;
  correction with recorded fallback; exact mode validated against the reference.
* **Done:** [verified here] the substrate, candidate manager, verification engine, correction
  engine and LPQ are implemented and tested, and exact LPQ matches the reference in all tests.
  Two substrate defects were found by fuzzing and fixed with regression tests. [doc] fallback
  counts recorded (7,119 of 13.8M pops reached the classical fallback on NY).
* **Remaining:** the learned component does not reduce end-to-end time on measured workloads.
  This is a research result, not a missing implementation, and it must remain visible. The
  remaining 10% is the performance target, which the project may not meet; see §4.

### M9 — Approximate mode — **75%**
* **Criteria:** user-supplied ε; ratio distribution (mean, median, p50/p95/p99, max, percent
  within each ε); reference check on every query; empirical versus formal label.
* **Done:** [verified here] the approximation report has max, p95, p99 and the within-ε logic.
  The benchmark aborts if any query breaks `1+ε` (`--mode approximate` requires `ε > 0`).
  [doc] ε = 0.1 removed 44% of comparisons on NY; the slack saturates by 0.01 on NY.
* **Remaining:** the measurement uses **5 queries** on NY and one random graph. A proper
  measurement needs the full 100-query manifest on several graphs, with the ratio distribution
  reported. The empirical-versus-formal label is correctly stated in the docs.

### M10 — Adaptive controller and fallback — **60%**
* **Criteria:** controller selects modes from measured gain; hysteresis and dwell; fallback
  works; controller overhead measured; controller's contribution separated from other levers.
* **Done:** [verified here] the controller has upper and lower thresholds and a dwell time, and
  the fallback is exercised.
* **Retracted claim (historical).** v1 reported that the adaptive configuration performed
  "34% fewer comparisons at identical pop counts" (116,608,480 vs 177,580,603) and called it the
  one place the learned machinery beat its own baseline. **That claim is retracted.** The v2
  audit reports that the same comparison count (116,608,480) is within 0.05% of the classical
  bucket queue's 116,552,995, which means the reduction came from retreating to a classical scan
  and not from learning [PR #1] `docs/experiments/v2-results.md` §3. The numbers 116,552,995
  and 116,608,480 are **[doc]**: I did not re-run this comparison. v2 does not validate the
  34% claim. It contradicts the attribution.
* **Remaining:** the controller's three levers are not separated. It adapts within a query, not
  across queries. The adaptive configuration is not shown to beat the classical bucket queue,
  so the controller's benefit is currently **zero measured**. It is not known whether
  the controller helps at all.

### M11 — Online learning — **25%**
* **Criteria:** warm and cold start; periodic and event-triggered updates; bounded update cost;
  updates from verified events; measured update latency; updates change model weights in a
  real run.
* **Done:** [verified here] `OnlineLearner`, `PredictionEvent` and EWMA are implemented and
  unit-tested in isolation. The update path is bounded (`max_weight_delta`, learning rate, L2).
* **Gap:** `maybe_update` and `flush` have no call site outside tests [verified here, `git grep`
  on both trees; the PR #1 audit agrees]. By that source evidence no benchmark or API run can
  change model weights; this was not additionally confirmed by logging weights during a run.
  Acceptance criterion PRD §25.5 is **not met**.
* **Remaining:** wire the learner into the search at a bounded interval; add a test that weights
  change during a real query; measure update latency; measure the effect (which may be negative).

### M12 — Datasets: DIMACS, SNAP, OSM, synthetic — **75%**
* **Criteria:** real DIMACS and SNAP graphs; OSM pipeline with pinned, checksummed input;
  synthetic families; provenance metadata; acquisition scripts.
* **Done:** [doc] six real graphs (NY, COL, FLA, PA, CA, Facebook) plus constructed graphs
  (`synth_ratio1_ny`; PR #1 adds twelve derived graphs). [verified here] 22 dataset metadata
  files are tracked under `datasets/metadata/`, including checksum and provenance files for the
  OSM extracts. OSM extraction code and its tests exist
  (`python/tests/test_osm.py` passes). Six synthetic families are in the generator; the
  Poisson-Voronoi one is an approximation, which the source says to read before citing.
* **Gap 1 — OSM acquisition:** `configs/datasets/osm_monaco.yaml` pins
  `monaco-260925.osm.pbf`. CI returned HTTP 404 for it on 2026-10-11. The same date is used for
  `osm_liechtenstein.yaml`, which I did not verify. From this sandbox the Monaco URL was reset
  by the egress proxy, so I could not confirm the 404 independently.
* **Gap 2 — no OSM benchmark:** OSM graphs were checked for exactness, but no OSM benchmark
  result is recorded in the docs.
* **Gap 3 — raw graphs are not tracked:** the NY, COL, FLA, PA and CA graphs must be acquired.
  This is by design, but it means a clean checkout cannot reproduce results offline.
* **M3 update:** Gaps 1 and 2 are addressed **for Monaco only**. `osm_monaco` now reads the
  openstreetmap.fr extract (7,592 nodes, 11,680 arcs; a different graph from the earlier 5,692-node
  one), pinned by a locally computed SHA-256, with the 1000-query manifest regenerated. Dijkstra over six
  queues and A* (haversine) were exact against the reference on all 1000 queries, which is a
  correctness check and **not** a performance benchmark. The pin holds only for the retained bytes, and
  Liechtenstein is untouched. See `DISCREPANCIES.md` D-01.
* **Remaining:** decide how the Monaco source bytes are kept durable (the publisher replaces the
  file daily); source Liechtenstein; record an OSM benchmark if wanted; run the other real
  datasets through acquisition and checksum verification.

### M13 — Experiments: full matrix, distribution shift, ablations — **45%**
* **Criteria:** full algorithm × queue × mode matrix on all datasets; distribution shift tested
  (different sources and targets, unseen graphs, sizes, weight distributions, modified graphs,
  road closures and additions, synthetic shifts); ablations covering all 14 items in PRD §21.
* **Done:** [doc] the NY matrix is recorded (six queues, exact, 100 queries, 11 repetitions).
  The generalisation sweep covers five road networks and one constructed graph. A leave-one-topology-out
  policy evaluation exists on PR #1. The ablation waterfall covers bucketing, the window and
  verification, and the trained scorer.
* **Gaps:**
  * *Weight distributions:* only heavy-tailed weights appear in the recorded docs. Log-normal,
    exponential and narrow-range sweeps are not recorded.
  * *Modified graphs:* no road closures, additions or other modifications were tested.
  * *Ablations not run* (PRD §21): the ladder does not isolate confidence, online learning,
    fallback on its own, feature subsets, small versus large model, or sensitivity to prediction
    error. The adaptive controller's levers are not separated. A candidate field `confidence`
    exists in `CandidateManager`, but the docs do not show that it gates extraction.
* **Remaining:** the items above. Each needs a pre-registered prediction, paired runs and
  reported effect sizes (CLAUDE.md, README §10).

### M14 — Statistical analysis and figures — **65%**
* **Criteria:** paired tests with effect sizes and distributions; plot scripts that consume
  stored results and fail on incomplete data; publication figures and tables generated from
  raw results.
* **Done:** [verified here] the statistics package and its tests pass (222 Python tests on
  `main`). The table and figure scripts exist and refuse incomplete results (documented in
  `docs/reproducing.md`). [doc] paired permutation tests with Holm correction and bootstrap
  intervals are used.
* **Remaining:** `results/raw` is git-ignored and absent from this checkout, so **no figure or
  table was generated or viewed in this session**. Generate them from a reproduction run, inspect
  them, and check each against its raw source. The `generate_figures` and `generate_tables`
  scripts are unverified in this state.

### M15 — Hardware profiling — **30%**
* **Criteria:** perf-based collection where available; cycles, instructions, branches, branch
  misses, cache references and misses, LLC misses; unavailable counters documented, never
  estimated.
* **Done:** [verified here] `scripts/profile/check_counters.sh` exists and decides which counters
  the host supports. [doc] unavailability is documented in `docs/experiments/profiling.md`, with
  native Linux and VTune as the alternatives. No counter has been measured anywhere.
* **Remaining:** collect counters on native Linux or with VTune on Windows. Until then this
  milestone stays at documentation only. The cache-miss hypothesis is untested.

### M16 — Reproducibility pipeline and paper artifacts — **70%**
* **Criteria:** one command from clean environment to figures; each script stops on a missing
  dataset, a wrong checksum, incomplete results or an absent metric; seeds explicit; environment
  pinned; reproduce from a clean clone.
* **Done:** [doc] `build_all.sh`, `run_main_experiments.sh`, `generate_tables.sh` and
  `generate_figures.sh` exist and carry the failure checks. [doc] a clean-clone run built 126
  targets and passed 831/831 tests (this was before the v2 changes). Seeds are explicit (42, 20260923).
  Python requirements are pinned.
* **Gaps:**
  * **Executable bits.** On the v2 head 20 files under `scripts/` are mode 100644 [verified
    here, `git ls-tree`]: 8 `.sh`, 7 `.ps1`, 5 `.py`. Eleven other scripts are 100755.
    The `.ps1` and `.py` files are run through their interpreter and need no bit. Of the 8 `.sh`
    files, 7 are documented as run directly: `scripts/build/linux_checks.sh`,
    `scripts/build/refresh_graph.sh`, `scripts/profile/check_counters.sh`,
    `scripts/reproduce/build_all.sh`, `generate_figures.sh`, `generate_tables.sh` and
    `run_main_experiments.sh`. The eighth, `scripts/reproduce/common.sh`, is only sourced. On the
    `main` checkout, `scripts/build/refresh_graph.sh` failed with *Permission denied* [verified
    here, first session]. The other six were not individually executed.
  * **No repository dependency conflict was found.** The first version of this document listed
    a `starlette` / `sse-starlette` conflict as a repository defect. That was an error.
    `sse-starlette` is not referenced anywhere in the repository (`git grep` on both refs).
    In the audit sandbox `pip show sse-starlette` reports it is required by the `mcp` package,
    which is not a project dependency; installing the project's `fastapi==0.118.0` (which
    resolves `starlette` 0.48.0) next to it produced pip's warning. A dry-run of the documented
    requirements (`python/requirements-dev.txt`, `atlas/api/requirements.txt`) on the v2 head
    would change nothing [verified here], and the CI `engine` job installs them and passes
    [CI]. Not reproduced in a clean virtual environment, which is a task for M10, so the
    statement is **inferred** until then.
  * Raw results and processed graphs are not in the repository.
  * The OSM step (M12) cannot run from the pinned URL.
* **Remaining:** *(M4 update, local branch `completion/m4-reproducibility-tooling` only: the 7
  scripts are now mode 100755 with a test, and the documented workflow was run end to end on
  `dimacs_ny` and `snap_facebook`; see `IMPLEMENTATION_PROGRESS.md` and D-13, D-19, D-20. The
  percentages in this document were not re-scored.)* Make the 7 directly-run shell scripts
  executable in git (a source change; done on the M4 branch, not on `main` or v2), repair the OSM step, confirm in a clean virtual environment that the
  requirements install without a conflict (M10), and run the full reproduction once from a clean
  clone with datasets acquired.

### M17 — Atlas backend (API) — **92%**
* **Criteria:** registry for datasets and models; source/target resolution; real engine jobs;
  background jobs; trace streaming; comparison and benchmark APIs; failures surfaced; no shell or
  path injection; contract tests between API and engine.
* **Done:** [verified here] 26/26 contract tests pass on `main`; 87 pass and 1 is skipped on PR #1.
  The contract tests cover exact and approximate LPQ, A*, unreachable pairs, malformed requests,
  and SSE equal to the trace file. [doc] no shell (argument lists from enums), validated
  identifiers, capped trace size and run time, and filesystem persistence. Structured JSON
  logging exists.
* **Remaining:** I did not independently audit the no-injection claim. Treat it as design intent
  until audited (0.5 on §52 in §3).

### M18 — Atlas frontend — **90%**
* **Criteria:** real map; snapping; Dijkstra and A* replay from the real trace; LPQ replay;
  exact/approximate switch changes the backend run; the final path is drawn from the returned
  path; metrics from results; compare mode; benchmark workspace; dataset, model and research
  pages; loading, empty and error states.
* **Done:** [verified here] lint has 0 errors on `main` and the build succeeds. [CI] PR #1
  lint, typecheck, unit tests and build are all green, and the Playwright e2e job passes on
  the latest commit. Pages exist for explore, compare, benchmarks, models, datasets, research,
  learn and runs.
* **Gaps:**
  * The e2e job uses the bundled `demo_road` dataset, not the OSM region the acceptance scenario
    (specdoc §59) names. This is because the OSM download is broken (M12).
  * 7 lint warnings on `main` (none are errors).
  * The loading and error states are not individually verified here.
* **Remaining:** merge the PR; clear the warnings; run the acceptance scenario on a real OSM
  region once M12 is fixed.

### M19 — Hardening: CI, fuzzing, security, cross-platform — **80%**
* **Criteria:** hosted CI covering build, tests, Python, API, web and contract tests; fuzzing;
  cross-platform builds; security; observability.
* **Done (two hosted runs, read through the GitHub API):**
  * [CI] [run #27](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38148343456),
    PR #1 head `e6dde1c`, 2026-10-11: `engine`, `web`, `fuzz`, `windows-scripts` and `e2e` all
    succeeded. The `e2e` job passes because the v2 workflow prepares the **bundled `demo_road`**
    graph (`ci.yml` line 114), not a real OSM region.
  * [CI] [run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184),
    the docs commit `b2049c2` on `claude/sleepy-heisenberg-fhr3me`, 2026-10-13, using `main`'s
    workflow: `engine` (which runs the C++, research-tooling and API contract tests), `web` and
    `fuzz` succeeded; **`e2e` failed** at step 7 "Prepare the OSM Monaco dataset (pinned
    snapshot, checksum-verified)" with `monaco-260925.osm.pbf: HTTP 404 Not Found`. The later
    `e2e` steps were skipped. This is an input-availability failure, not a test failure.
  * [doc] libFuzzer targets, and sanitiser runs on Clang recorded earlier.
* **Gaps:**
  * No hosted run on the `main` branch itself was among the 8 most recent runs inspected (the
    others were on the PR #1 and docs branches), so CI status of `main` is **[untested]**. The
    workflow file exists on `main` and triggers on every push.
  * `main`'s workflow always fails at the OSM step today, so a push to `main` would show red
    until M3. The PR #1 workflow avoids this only by using `demo_road`.
  * The Windows engine build is not in CI. v2's `windows-scripts` job parses PowerShell and
    runs the read-only doctor, and is marked `continue-on-error: true`. Its history includes
    several failed attempts before the final green run.
  * The fuzz job is bounded (about 4 minutes); long fuzzing is not run.
* **Remaining:** repair the OSM step (M3) or otherwise make `e2e` independent of the dead
  URL, merge the PR so CI runs on `main`, add a Windows engine build job, and lengthen fuzzing
  on a schedule.

### M20 — Research documentation and abstract alignment — **75%**
* **Criteria:** abstract revised to describe measured work; negative results visible; no
  cache-miss claims; no "expected outcome" language; no claim that LPQ universally outperforms;
  literature review and novelty statement current.
* **Done:** [doc] the revised abstract states the mixed results; the discrepancy register is
  thorough; the 34% retraction is recorded in PR #1's `v2-results.md` §3 and the PR description.
  PR #1 adds `docs/research/literature-and-novelty.md`, which positions the work against
  Benomar and Coester (NeurIPS 2024).
* **Gaps:**
  * The 34% adaptive claim is **still stated, un-retracted, in four documents** on `main`, on
    the PR #1 branch and on the docs branch: `experiments/paper/findings.md`,
    `docs/atlas/ABSTRACT.md`, `docs/experiments/adaptive-controller.md` and
    `docs/experiments/README.md` [verified here, `git grep`]. Merging PR #1 would **not** fix
    this. **M2 annotates all four on this branch only**, leaving the original text.
  * The `docs/atlas/README.md` and `ABSTRACT.md` wording about the adaptive controller and
    "online" learning describes components that are implemented but, per the v2 audit and
    `git grep` on both branches, not wired into any run (`maybe_update`/`flush`). M2 adds
    a status note in `DISCREPANCIES.md` (D-02) and does not rewrite those sentences.
  * D-06 (CI "not yet run") was stale; updated on this branch (M2).
  * A **fresh** literature review (PRD §27) has not been done. The v2 note is a first pass, and its
    own text flags some statements as unverified.
* **Remaining:** carry the M2 annotations into any branch that is eventually merged; complete
  the literature review before any publication claim.

### M21 — Integration and release — **0%**
* **Criteria:** the v2 work is reviewed and merged into the default branch; the release builds
  and tests from a clean clone; the documented state matches the code.
* **Done:** PR #1 is open, a **draft**, and `mergeable_state: clean`. Its code builds and passes
  its tests here, and its CI is green.
* **Remaining:** review, undraft and merge PR #1 (a human decision), then re-run the verification
  from a clean clone of `main`.

---

## 3. Acceptance matrices

Scores: 1 = met, 0.5 = partly met (gap named), 0 = not met.

### 3.1 PRD §25 — technical completeness (14 criteria) — **64%**

| # | criterion | score | basis |
|---|---|---:|---|
| 1 | All baseline queues implemented and validated | 1 | binary, 4-ary, pairing, Fibonacci, radix; differential tests |
| 2 | LPQ returns correct exact results in exact mode | 1 | reference check in every benchmark; tests |
| 3 | Approximate mode produces measured approximation ratios | 0.5 | measured on 5 queries and one random graph |
| 4 | Adaptive fallback works | 0.5 | fallback runs and is counted; the adaptive mode is not shown to help |
| 5 | Online adaptation works | 0 | `maybe_update` / `flush` have no call site outside tests |
| 6 | ML overhead measured end to end | 0.5 | static scorer build cost reported separately and excluded from the timed clock |
| 7 | Tests cover queue invariants and algorithmic correctness | 1 | 914/914 on PR #1; 847/847 on `main` |
| 8 | Results are reproducible | 0.5 | comparison counts reproduce; timings and raw data do not ship; OSM step broken |
| 9 | Experiments run on synthetic and real graphs | 1 | DIMACS, SNAP, synthetic, derived |
| 10 | Distribution-shift behaviour evaluated | 0.5 | cross-graph generalisation only; no closures, modifications or weight sweeps |
| 11 | Ablations identify the contribution of every major component | 0.5 | waterfall covers three components; §21 items missing |
| 12 | Performance claims rest on statistically meaningful repeated experiments | 1 | paired, pinned, median/MAD, permutation and bootstrap |
| 13 | System produces publication-quality figures and tables | 0.5 | scripts exist; none generated or inspected here |
| 14 | No novelty claim contradicts existing literature | 0.5 | abstract avoids the claim; a fresh review is pending |
| | **Total** | **9.0 / 14** | **64%** |

### 3.2 ESD §40 — engineering definition of done (12 criteria) — **79%**

| # | criterion | score | basis |
|---|---|---:|---|
| 1 | All modules compile independently | 1 | header self-containment check in CMake |
| 2 | Tests pass | 1 | 847 / 914 |
| 3 | Differential tests pass | 1 | 24 tests |
| 4 | LPQ exact mode matches reference | 1 | tests and benchmark check |
| 5 | Approximate mode reports measured ratios | 1 | runner reports ratios; limited sample |
| 6 | Online updates work | 0 | not wired |
| 7 | Fallback works | 1 | tests; counted in runs |
| 8 | Benchmark runner produces machine-readable results | 1 | JSON records |
| 9 | Plot scripts consume those results | 0.5 | scripts exist; not exercised on real results here |
| 10 | Reproduction scripts work from a clean environment | 0.5 | verified on an earlier clean clone; OSM step broken |
| 11 | No result generated without hardware and git metadata | 0.5 | hardware info is designed into every record; not re-checked per record type |
| 12 | No performance claim without a baseline comparison | 1 | every claim is against a baseline |
| | **Total** | **9.5 / 12** | **79%** |

### 3.3 README §19 — success checklist (18 items) — **69%**

| # | item | score | basis |
|---|---|---:|---|
| 1 | Exact correctness | 1 | §3.1 #2 |
| 2 | Approximation behaviour measured | 0.5 | 5 queries |
| 3 | Classical baselines implemented | 1 | §3.1 #1 |
| 4 | LPQ benchmarked fairly | 1 | paired, pinned, same queries |
| 5 | ML overhead measured | 0.5 | static scorer excluded from the timed clock |
| 6 | Verification overhead measured | 1 | phase profile |
| 7 | Online learning measured | 0 | not active |
| 8 | Fallback behaviour tested | 1 | tests and counts |
| 9 | Synthetic graphs tested | 1 | generator and benchmarks |
| 10 | SNAP tested | 1 | PA, CA, Facebook |
| 11 | OSM tested | 0.5 | exactness checked earlier; acquisition broken; no benchmark |
| 12 | Unseen graphs tested | 0.5 | cross-graph and leave-one-topology-out |
| 13 | Distribution shift tested | 0.5 | see §3.1 #10 |
| 14 | Ablation study completed | 0.5 | see M13 |
| 15 | Statistical analysis completed | 1 | paired tests with effect sizes |
| 16 | Hardware profiling completed | 0.5 | unavailability documented; no counter measured |
| 17 | Reproduction scripts work | 0.5 | see §3.2 #10 |
| 18 | Literature review updated | 0.5 | first pass on PR #1; fresh review pending |
| | **Total** | **12.5 / 18** | **69%** |

### 3.4 specdoc §51–55 — Atlas definition of done (57 items) — **93%**

| section | items | score | pct | main gaps |
|---|---:|---:|---:|---|
| §51 Research engine | 10 | 9.5 | 95% | model metadata not embedded (§30) |
| §52 Backend | 10 | 9.5 | 95% | injection-free claim not independently audited |
| §53 Frontend | 14 | 13.0 | 93% | OSM map depends on broken acquisition; loading states unverified |
| §54 OSM | 12 | 11.0 | 92% | **reproducible PBF acquisition: 0** (pinned URL 404) |
| §55 Research integrity | 11 | 10.0 | 91% | the retracted 34% claim is still stated in four documents on every branch except this one; charts not traceable to stored raw results here |
| | **57** | **53.0** | **93%** | |

---

## 4. Known research gaps that may not close

These are not implementation tasks. The acceptance criteria require that they be *measured and
reported*, whatever the result. The project may finish with a negative answer on them.

1. **Does learning reduce end-to-end runtime?** On the measured workloads it does not, apart from
   the NY Dijkstra result, which comes from queue engineering. A better model cannot close this
   on its own: the measured cost is in verification and window maintenance (about 63–76% of
   extraction time).
2. **Does online learning help?** It is not running. Once it is, the result may be neutral or
   negative. Both are acceptable outcomes if reported.
3. **Does the adaptive controller help?** Currently no measured benefit. The 34% claim is
   retracted (historical; see M10 and D-11).
4. **Does the learned model generalise?** Held-out AUC is 0.79 on Facebook (pre-correction protocol; 0.80 under the disjoint protocol,
   D-19) and 0.53 on NY. The
   leave-one-topology-out policy never beat radix on any held-out graph.
5. **Cache and branch behaviour.** Not measurable on the development host.

The success definition in PRD §29 does not require LPQ to be faster. It requires a
*reproducible, measured, honest* account of when and why it helps. Under that definition the
project is already close to done on the measurement side. It is far from done on the
online-learning and ablation side.

---

## 5. Document corrections (verified discrepancies)

Status column: **done on this branch** means M2 changed the file on `completion/v2-baseline`;
the same file is unchanged on `main`, the docs branch and the PR #1 branch.

| # | where | says | verified state | status |
|---|---|---|---|---|
| 1 | `docs/atlas/DISCREPANCIES.md` D-06 | CI "not yet run on GitHub" | hosted runs exist: #27 green on PR #1, #28 red at the OSM step | done on this branch |
| 2 | `experiments/paper/findings.md`, `docs/atlas/ABSTRACT.md`, `docs/experiments/adaptive-controller.md`, `docs/experiments/README.md` | the adaptive controller removes 34% of comparisons; learned machinery improves on its baseline | retracted: the figure equals the plain bucket queue [doc: `v2-results.md` §3]. **v2 does not edit these four files** | annotated on this branch (retraction notice, original text kept); **open** on every other branch |
| 3 | `docs/atlas/DISCREPANCIES.md` D-02 | "online ML" wording is resolved in the UI | `OnlineLearner::maybe_update` and `flush` have no call site outside tests on `main` and on the PR #1 branch [verified here, `git grep`] | done on this branch |
| 4 | `docs/atlas/DISCREPANCIES.md` D-01 | OSM "Resolved" | pinned Monaco snapshot returns HTTP 404 in CI (2026-10-11 and run #28, 2026-10-13); Liechtenstein untested | reopened on this branch |
| 5 | PR #1 description | Python 311 passed; API 77 passed | observed 300 passed + 12 skipped; 87 passed + 1 skipped | recorded in `IMPLEMENTATION_PROGRESS.md` with proposed text; **PR not edited** (out of scope for M2) |
| 6 | git file modes of 7 `.sh` files | documented as run directly | tracked as 100644 on `main` and on v2 | recorded as D-13; **fix deferred to M4** (source change) |
| 7 | this document, first version: `atlas/api/requirements.txt` | pip conflict between `starlette` and `sse-starlette` | not a repository defect: `sse-starlette` is absent from the repo; the sandbox's `mcp` package requires it | corrected here; recorded as D-16 |
| 8 | this document, first version: "PR #1 documents supersede the 34% claim; corrected on merge" | | false: v2 edits none of the four documents | corrected here |

---

## 6. What remains, in priority order

Each item states the acceptance criterion it closes and how to know it is done.

### Priority 1 — blocks the acceptance criteria

1. **Wire online learning into the search.** Call the learner at a bounded interval from real
   queries (warm and cold start). *Done when:* a test shows weights change during a real query,
   the update latency is recorded, and a benchmark run reports the effect with its sign.
   *Closes:* PRD §25.5, ESD §40 #6, README §19 #7.
2. **Repair the OSM acquisition.** *Monaco: done in M3 on its branch, with a durability limit*
   (see D-01). *Open:* a durable home for the Monaco bytes, and Liechtenstein (deferred by the
   owner). *Done when:* both datasets prepare from a clean checkout using bytes whose SHA-256
   matches the config pin. *Closes:* specdoc §54 (reproducible PBF), README §19 #11.
3. **Full approximate-mode measurement.** Run the 100-query manifests on several graphs at
   several ε values. Report mean, median, p50, p95, p99, maximum, and the percent within each ε.
   *Closes:* PRD §25.3, README §19 #2.
4. **Include the ML cost in the timed clock.** Measure feature extraction and inference per query,
   not only the precomputed scorer build. *Closes:* PRD §25.6, README §19 #5.
5. **Run the missing ablations** (PRD §21): learned without adaptation; + confidence; + fallback
   alone; feature subsets; small versus large model; sensitivity to prediction error. Pre-register
   each prediction before running it. *Closes:* PRD §25.11, README §19 #14.
6. **Distribution shift.** Add the log-normal, exponential and narrow-range weight sweeps. Add road
   closures, additions and modifications. Add size scaling. *Closes:* PRD §25.10, README §19 #13.

### Priority 2 — needed to call the work reproducible and published

7. **Merge PR #1** after review, then undraft it. Re-run the verification from a clean clone of
   `main`. *Closes:* PRD §25.8 (partly), M21.
8. **Carry the M2 documentation corrections into the branch that is eventually merged**
   (items 1 to 4 and 8 in §5). *Status:* done on `completion/v2-baseline`; merging PR #1 alone
   would not do it.
9. **Embed model metadata in the artifact**: checksum, seed, training-query identifier, feature
   schema, hyperparameters, metrics (specdoc §30). *Closes:* §51 and §30 gaps.
10. **Decide the MLP and tree models**: evaluate them on the task, or record that they are dropped
    under the simplest-model rule (PRD §10). *Closes:* PRD §21 item 9.
11. **Generate and inspect the figures and tables** from a reproduction run. Check each against its
    raw source. *Closes:* PRD §25.13, ESD §40 #9.
12. **Make the 7 directly-run `.sh` scripts executable** in git (`git update-index
    --chmod=+x`; done on the local M4 branch, commit `ee8d78f`), and confirm in a clean virtual environment that the documented requirements
    install without a conflict (the earlier "pip conflict" is not a repository defect; see
    M16 and D-16). *Closes:* M16.
13. **Fresh literature review** before any publication claim. *Closes:* PRD §25.14, README §19 #18.

### Priority 3 — hardening and platform

14. **Hardware counters** on native Linux or VTune. Until then, keep the documentation as the
    record. *Closes:* PRD §20 (where available), README §19 #16.
15. **Windows engine build in CI.** *Closes:* specdoc §40 and M1.
16. **Run the §59 acceptance scenario** on a real OSM region once item 2 is done. The current
    e2e uses `demo_road`.
17. **Clear the 7 web lint warnings** on `main`.
18. **Audit the no-injection claim** in the API (specdoc §52).
19. **Longer fuzz runs** on a schedule.
20. **Verify the loading and error states** in the UI, item by item (specdoc §53).

---

## 7. What was verified in this session

| check | command or source | result |
|---|---|---|
| Engine configure, build, ctest (`main`) | `cmake --preset release`, `--build`, `ctest --preset release` | 147 targets, 0 warnings; 847/847 |
| Engine configure, build, ctest (PR #1) | same, in a separate worktree in the scratchpad | 155 targets, 0 warnings; 914/914 |
| Python tests (`main`) | `python -m pytest -q` in `python/` | 222 passed, 8 skipped |
| Python tests (PR #1) | same, in the worktree | 300 passed, 12 skipped |
| API contract tests (`main`) | `python -m pytest -q` in `atlas/api/` | 26 passed |
| API contract tests (PR #1) | same, in the worktree | 87 passed, 1 skipped |
| Web on `main` | `npm ci`, `npm run lint`, `npm test`, `npm run build` | lint 0 errors / 7 warnings; tests 5/5; build exit 0 |
| Web on the v2 head (M1) | `npm ci`, `npm run lint`, `typecheck`, `npm test`, `npm run build` in a clean worktree | all exit 0; lint 0 errors / 7 warnings; unit tests 41/41; `npm ci` reports 11 audit vulnerabilities (1 moderate, 8 high, 2 critical), not acted on |
| End-to-end on the v2 head (M1) | `scripts/demo/prepare_demo.py`, API + `next start`, `npx playwright test` (preinstalled Chromium via `ATLAS_CHROMIUM_PATH`) | exit 0; 8 passed, 2 skipped (missing `snap_roadnet_pa`; missing NY results/model) |
| Hosted CI, PR #1 `e6dde1c` | GitHub Actions API | [run #27](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38148343456): `engine`, `web`, `fuzz`, `windows-scripts`, `e2e` all succeeded (`e2e` on `demo_road`) |
| Hosted CI, docs commit `b2049c2` | GitHub Actions API, job log tail | [run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184): `engine`, `web`, `fuzz` succeeded; `e2e` failed at step 7, `monaco-260925.osm.pbf: HTTP 404 Not Found` |
| PR #1 state | GitHub pull request API | open, draft, base `main`, head `e6dde1c`, mergeable state clean (re-read 2026-10-13) |
| Branch ancestry | `git merge-base`, `git diff --name-only` | docs commit and v2 share merge-base `4e7ad00` and change disjoint files; cherry-pick of `b2049c2` applied cleanly |
| Online learner call sites | `git grep` for `maybe_update` / `flush` on both branches | defined at `online_learner.hpp:246` and `:259`; referenced only by `cpp/tests/adaptation/test_online_learner.cpp` |
| `confidence` field | `git grep` over `cpp/include`, `cpp/apps` on v2 | declared (`candidate_manager.hpp:108`) and copied into `PredictionEvent`; produced by predictors; not read to gate extraction in `lpq/`, `verification/`, `adaptation/` or the apps |
| `sse-starlette` | `git grep` on both refs; `pip show sse-starlette` | absent from the repository; required by `mcp` in the audit sandbox |
| Script modes | `git ls-tree -r HEAD -- scripts` | 20 at 100644 (8 `.sh`, 7 `.ps1`, 5 `.py`); 11 at 100755 |
| Graph index | `scripts/build/refresh_graph.sh` (run through `bash`) | 4,492 nodes, 9,385 edges, 218 communities (code-only) |
| OSM Monaco URL | `curl -I` | connection reset by the egress proxy; the 404 is from the PR #1 config comment, not re-observed here |
| Model metadata | read `python/lpq_tools/training/model_io.py` and `train.py` | artifact has no checksum, seed or metrics; the report does |

**Not verified here / skipped / not run [untested]:** the Windows build; any hardware counter;
any figure or table; OSM acquisition from the pinned source (the sandbox cannot reach Geofabrik,
and the 404 is CI-observed); Liechtenstein availability; the libFuzzer job; the Debug
ASan/UBSan preset; `ctest --preset release-fast`; any benchmark; the reproduction scripts; the
12 Python and 3 UI/API tests that skip for missing data or by design; repeated runs (every
count above is a single execution).

---

## 8. Limits of this assessment

* The percentages are judgements against stated criteria, and I have labelled each judgement
  with its basis. A different rubric would move the numbers, but not the verdict: PRD §25 has
  three unmet or partly met items that are binding.
* I did not re-run the experiments. The comparison counts and timings in the docs are cited,
  not reproduced.
* The graph index was built code-only. Doc files are not in it.
* This document was first written from the `main` checkout and revised in M2 on the local
  branch `completion/v2-baseline`. Neither version was pushed. The percentages were not
  re-scored in M2 (see the revision note).
