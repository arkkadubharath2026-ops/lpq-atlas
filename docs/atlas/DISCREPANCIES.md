# Discrepancy register: resolution status

Spec §5 lists ten discrepancies between the abstract, the documentation and the code.
This file records what LPQ Atlas did about each, and the further defects found while
building it. Nothing below was removed to make the documentation look consistent.

## Spec items

| id | item | status | evidence |
|---|---|---|---|
| D-01 | OSM promised, not implemented | **Monaco: re-sourced and verified (M3, 2026-10-13). Durable retention of the source bytes, and Liechtenstein: still open.** `osm_monaco` now reads the openstreetmap.fr "monaco" extract (replication state 2026-10-13T01:14:05Z, sequence 7326506), pinned by SHA-256 `26ac807383921434f6fc7ef07ddb367e03c8357e65ac360c72d5de120c239d1d`, computed locally by `sha256sum`, Python `hashlib`, `openssl` and the repo's helper (all equal). The publisher's MD5 `df804c9f8e6da26ddb350af60132554c` matched; it only detects corruption, so the pin is **trust-on-first-use**. Verified: publisher headers, MD5 file and state identical before and after the download; the PBF header's replication timestamp and sequence equal `state.txt`; two separate downloads and the pipeline's own fresh download are byte-identical; all seven pipeline outputs (graph, node-id map, four metadata files, 1000-query manifest) are byte-identical across a retained-bytes run and a fresh-download run; flipped bytes and the old Geofabrik digest are rejected; the runner refuses the old manifest. On the 1000-query manifest, Dijkstra over six queues and A\* with haversine are exact against the reference (correctness only, no performance claim), and LPQ approximate at ε=0.1 recorded ratio 1.0 on all queries. **It is a different graph:** 7,592 nodes and 11,680 arcs (earlier 5,692 and 8,495), bbox [7.4053929, 43.7232244, 7.4472402, 43.7545284] against [7.4053929, 43.7232244, 7.4423019, 43.7519682], about 1.23x the area; 32 road segments reference nodes cut at the extract boundary. The raw extract also holds nodes far outside Monaco (latitude 37.27 to 43.78, no header bbox); none enter the road graph. **Limit:** the publisher replaces the file daily and keeps no dated copies, so the pin holds only for these bytes; reproduction needs the retained file with `--skip-download` or a re-pin. Not redistributed (ODbL obligations need a separate decision). The tracked lock file `datasets/metadata/osm_monaco.checksums.json` still records the old Geofabrik digest and was left unchanged. Liechtenstein still names the dead Geofabrik URL (deferred by the owner). History: **Reopened (M2, 2026-10-13); originally recorded as Resolved.** The extraction code and its tests exist, but the pinned dated Geofabrik snapshot can no longer be downloaded: `monaco-260925.osm.pbf` returned `HTTP 404 Not Found` in CI on 2026-10-11 and again on 2026-10-13 ([run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184), step "Prepare the OSM Monaco dataset"). Liechtenstein uses the same snapshot date and is untested. The v2 workflow avoids the download by using the bundled `demo_road`, so v2's green `e2e` says nothing about real OSM. Original evidence follows. | `python/lpq_tools/data/osm.py` (documented road policy), `configs/datasets/osm_*.yaml` (dated Geofabrik snapshots, SHA-256 pinned), `datasets/metadata/osm_*.osm.json` (provenance), `python/tests/test_osm.py`; Dijkstra, A* (haversine) and LPQ verified exact on `osm_monaco` / `osm_liechtenstein` |
| D-02 | "online ML" broader than the central path | **Resolved in the UI for the scorer label; reopened for online learning (M2, 2026-10-13); the label and a between-query `lpq_online` queue are done in M5 O1 to O4, see D-12.** `OnlineLearner::maybe_update` and `flush` have no call site outside tests on `main` and on the PR #1 branch (`git grep`, 2026-10-13): non-test code only calls `record()` (`controller_observer.hpp:99`) and `reset()`, so no model weight changes in any benchmark or API run. See D-11 and D-12. Original evidence follows. | every result states `lpq.scorer`, `adaptive_controller`, `online_learning`; the Models page states the static-scorer mode |
| D-03 | rich features vs optimized path | **Exposed** | `lpq.features` lists the six static features used; the Model page shows which weights are non-zero as read by the C++ loader |
| D-04 | model zoo vs selected model | **Resolved** | Models page separates implementations, trained artifacts, the queue that uses them, and split status |
| D-05 | cache misses | **Unavailable, stated** | `hardware_counters.available = false` with the reason, in every result and on every page that would show them |
| D-06 | no hosted CI | **Updated (M2, 2026-10-13); originally recorded as "Written; not yet run on GitHub". Hosted CI has since run; it is green on the v2 workflow and red on the v1 workflow.** [Run #27](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38148343456) on PR #1 `e6dde1c`: `engine`, `web`, `fuzz`, `windows-scripts`, `e2e` all succeeded (the `e2e` job uses `demo_road`). [Run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184) on the docs commit `b2049c2` with `main`'s workflow: `engine`, `web`, `fuzz` succeeded, `e2e` failed at the OSM Monaco download with `HTTP 404`. The Windows job parses PowerShell and runs a read-only doctor; there is no Windows engine build. No run on the `main` branch itself was inspected. The text below is what was recorded before any hosted run existed. | `.github/workflows/ci.yml`: engine build + ctest, Python, API contract, libFuzzer, web lint/typecheck/tests/build, Playwright on OSM Monaco. Nothing has been pushed, so the hosted run is pending. Its steps were run locally on Linux (WSL Ubuntu 26.04): GCC 15.2 and Clang 21 Release `-Werror` and Clang Debug ASan+UBSan+LSan, each 847/847 with zero warnings; libFuzzer 60 s per target, no findings after defect 6's fix. The full stack also runs from `compose.yaml` (Playwright 5/5 against the containers) |
| D-07 | no formal fuzzing | **Resolved** | `cpp/fuzz/`: graph binary reader, model loader, experiment manifest reader, queue operation sequences; libFuzzer with clang, standalone smoke runs in ctest (`fuzz-smoke`). It found two real defects (1 and 6 below) |
| D-08 | checked-in / absolute-path build state | **Resolved** | `build/` is ignored; the local `build/release` was configured from a different absolute path and could not rebuild, so it was set aside and the release preset reconfigured from source |
| D-09 | expected outcome vs findings | **Visible** | Compare and Research never declare a winner; paired statistics report effect size and fraction of queries beside Holm-adjusted p-values; the revised abstract is [ABSTRACT.md](ABSTRACT.md) |
| D-10 | train/eval leakage history | **Automated** | `python/lpq_tools/training/leakage.py`; `test_leakage.py` asserts the committed NY training set is disjoint from both evaluation manifests; every model shows `Training/Evaluation split: DISJOINT` or a warning |

## Defects found while building LPQ Atlas

### 1. Bucket substrate returned keys out of order past its window (correctness)

`BucketSubstrate` parks keys beyond `base + 1024 × width` in an overflow list and
re-filed them only after a full lap of empty buckets. When the window slid forward
without such a lap, a parked key the window now covered stayed parked, later pushes with
larger keys went into buckets, and those were extracted first. In a search that is a
**non-optimal distance in exact mode** (NY, width 1: 1,154,372 returned vs 1,139,254
exact). Found by `fuzz_queue_ops` on its first run (seed 20260926); confirmed end-to-end
with `lpq_run --lpq-bucket-width 1|2|4`.

*Fix:* the substrate now keeps the invariant "every overflow key is at or beyond the
window end", re-filing covered entries whenever the base advances
(`absorb_overflow`). Regression tests: `AnOverflowKeyTheWindowCatchesUpWithIsNotOvertaken`,
`StaysOrderedWhenTheWidthIsFarBelowTheKeyRange` (both fail on the old code).

*Effect on stored results: none.* The window only overflows when the bucket width is
below max-weight / 1024. Every recorded experiment used the derived width (= ceil(max
weight / 1024)) or wider (500, 5,000 on NY; ≥ 5 on random graphs with weights ≤ 1000), so
no key ever exceeded the window. In addition, `lpq_benchmark` validates every query
against the reference and aborts on a mismatch, so no wrong distance could have been
recorded.

### 2. Overflow re-filing discarded learned scores

`redistribute_overflow` re-pushed entries without their score, silently resetting it to
zero. Fixed; regression test `RefilingFromTheOverflowListKeepsTheScore`. Same scope as
above: unreachable at the derived width.

### 3. Haversine A* was given the Euclidean scale

`lpq_benchmark --heuristic haversine` passed `euclidean_admissible_scale` (weight per
*degree*) to a heuristic that multiplies *metres*, overestimating by orders of magnitude
and breaking optimality. No recorded experiment used haversine (A* results used
`euclidean`). Added `haversine_admissible_scale` and used it in both runners.

### 4. DIMACS coordinates labelled "geographic" were microdegrees

`lpq_convert` stored `.co` values as-is (microdegrees) under `CoordinateSystem::kGeographic`,
whose contract is degrees. Haversine on those values stayed admissible (the scale is
derived) but collapsed toward zero, turning A* into Dijkstra. `lpq_convert` now converts
to degrees after any weight synthesis (so synthesized weights are unchanged);
`haversine_admissible_scale` refuses out-of-range coordinates. All coordinate datasets
were reconverted; their arc checksums are unchanged (coordinates are not in the
checksum) and their query manifests are byte-identical. Euclidean A* is scale-invariant
(its scale is derived from the same coordinates), so recorded A* results are unaffected
up to integer rounding of the heuristic.

### 5. Python header reader lagged the graph format

`binary_header.py` accepted only format version 2 after the C++ writer moved to version
3, so any newly converted graph failed the pipeline's header check. It now reads 2 and 3
and reports the coordinate system.

### 6. Binary graph reader trusted the header's counts (robustness)

`read_binary_graph` sized its offset array from the declared node count and reserved the
declared arc count before reading any payload. A 72-byte file declaring 654 million nodes
requested 5.2 GB (libFuzzer out-of-memory in `fuzz_graph_binary`, first libFuzzer run on
Linux, seed 20260926); an arc count near 2^64 escaped as `std::length_error` instead of
`GraphIoError`, bypassing every caller that catches `LpqError`.

*Fix:* the declared counts are checked against the bytes remaining in the stream before
any allocation; offsets must start at 0 and never decrease; a stream that cannot report
its size (a pipe) grows its arrays only as bytes arrive. Regression tests:
`AHeaderClaimingMoreNodesThanTheFileHoldsIsRejectedBeforeAllocating` (the fuzzer's input,
embedded) and `AnArcCountNearTwoToTheSixtyFourIsAGraphIoError`, both failing on the old
reader, plus two unseekable-stream tests.

*Effect on stored results: none.* Every graph the project loads was written by
`lpq_convert` and passes the new checks unchanged (all datasets load, all suites pass).

## Findings added by the M0 to M2 audit (2026-10-13)

Added on the local branch `completion/v2-baseline`. Each entry says how it was checked. "Verified"
means a command was run in the audit sandbox on the commit named; "documented" means taken from
a project document and not re-run. No code, workflow or benchmark was changed to make any of these
entries true.

### D-11. The 34% adaptive-controller claim is retracted but still stated in four documents

* **Claimed:** `experiments/paper/findings.md` ("The adaptive controller is the one place the
  machinery pays"), `docs/experiments/adaptive-controller.md`, `docs/experiments/README.md` and
  `docs/atlas/ABSTRACT.md` say the adaptive configuration does 34% fewer comparisons and is the
  one case where the learned machinery improves on its own baseline.
* **Actual (documented):** `docs/experiments/v2-results.md` §3 reports that the classical bucket
  queue alone performs 116,552,995 comparisons against the adaptive configuration's 116,608,480
  (0.05% apart), so the reduction is attributed to retreating to a classical scan, not to
  learning. These figures were not re-run in the audit.
* **Verified:** v2 does **not** edit any of the four documents (`git diff --name-only` between
  `main` and `e6dde1c`). The claim is therefore un-retracted in those files on `main`, on the PR #1
  branch and on the docs branch. v2 does not validate the claim.
* **Status:** historical and retracted. Dated retraction notices were added to all four files on
  this branch; the original text is unchanged. **Open:** any other branch still states the claim
  without a notice. A single interleaved re-run of `lpq_adaptive` against the classical bucket
  queue would settle the attribution; it was not done and no figure was invented.

### D-12. The `online_learning` label can read true while no learning happens

* **Claimed:** each result states `lpq.online_learning`; the UI shows an "online learning" badge
  when it is true (`atlas/web/components/RunPanels.tsx:139`). D-02 recorded this as resolved.
  `v2-results.md` §3 says "Online learning is therefore not claimed anywhere".
* **Actual (verified, `git grep` on `e6dde1c`):** `lpq_run.cpp:777` sets `online_learning` to
  `pq == "lpq_adaptive" && adaptive_learning`, that is, true whenever the learner is *attached*
  with `--adaptive`. The learner is only ever `record()`ed and `reset()` by non-test code;
  `maybe_update` and `flush` have no call site outside tests. So the flag and badge can claim
  online learning on runs where no model weight changes. Separately, v1 documents
  (`adaptive-controller.md`, `findings.md`) do claim the learner ran inside a real search, which
  contradicts v2's "not claimed anywhere".
* **Status (updated in M5, O1 to O4; local branch `completion/m5-online-o1-o4`):** the **label** is
  corrected and the **mechanism** is wired for one queue; in-search updates and the benchmark/API/UI
  exposure of that queue are not done yet (O5 to O7).
  * *O1, label:* every `lpq` result now carries a `learner` block (attached, events recorded, examples,
    updates applied, rollbacks, halted, measured weights change with start and end hash), and the legacy
    `online_learning` flag is derived from it: true only if updates were applied **and** the weights differ.
    For `lpq_adaptive --adaptive-learning` the flag is now false, the block says "attached, events recorded,
    0 updates", and a note says no model exists there to update. The web badge has five states plus two for
    older results (which carry only the old flag and are shown as "learner attached; learning not verified").
  * *O2 to O4, mechanism:* a new `lpq_online` queue scores pushes with the live model, records verifier-labelled
    events (event contract version 2) and applies them **between queries** (`OnlineSession::end_query`, the first
    non-test call site of `OnlineLearner::flush`). `lpq_adaptive` and its historical results are unchanged.
  * *O6, O7 (core path, same branch):* `lpq_benchmark` runs `lpq_online` only with an explicit
    `--evaluation online_session`, and the API and run form expose it with an explicit model and recorded overrides.
    Still **not done**: in-search updates (O5, deferred as not required), API benchmarks of `lpq_online` (refused),
    Playwright coverage of the new UI (not run).
  * *Not claimed:* that an update improves anything. Nothing measures that yet, and the result says so.
  * *Still open:* v1 documents that say the learner ran inside a search (D-11) are annotated, not rewritten.

### D-13. Seven documented-as-executable shell scripts are tracked as mode 100644

* **Claimed (verified by `git grep` of the script paths):** `CLAUDE.md` names
  `scripts/build/refresh_graph.sh`; `ESD.md` §36 and `docs/reproducing.md` name
  `scripts/reproduce/{build_all,run_main_experiments,generate_tables,generate_figures}.sh`;
  `docs/reproducing.md` also names `scripts/build/linux_checks.sh`, and
  `docs/experiments/profiling.md` names `scripts/profile/check_counters.sh`. They are presented
  as commands to run, not as `bash <script>`. The README does not mention them.
* **Actual (verified, `git ls-tree -r`):** on `main` and on `e6dde1c` the following are mode
  100644: `scripts/build/linux_checks.sh`, `scripts/build/refresh_graph.sh`,
  `scripts/profile/check_counters.sh`, `scripts/reproduce/build_all.sh`, `generate_figures.sh`,
  `generate_tables.sh`, `run_main_experiments.sh`. (`scripts/reproduce/common.sh` is also 100644
  but is only sourced.) 20 files under `scripts/` are 100644 on v2: these 8 `.sh`, 7 `.ps1`
  and 5 `.py`; the `.ps1` and `.py` files are run through an interpreter. Eleven others are 100755.
  Direct execution of `scripts/build/refresh_graph.sh` failed with *Permission denied* on `main`;
  the other six were not individually executed.
* **Status:** **fixed in M4** (commit `ee8d78f`, branch `completion/m4-reproducibility-tooling`).
  The seven scripts are now mode 100755 with no content change; `python/tests/test_script_modes.py`
  reads the mode from the git index and failed on all seven before the change. The fix exists only on
  that local branch; `main`, v2 and the docs branch still carry mode 100644. Recorded earlier as
  "fix deferred to M4 because it is a source (file mode) change."

### D-14. `confidence` is produced and recorded but does not gate extraction

* **Claimed:** PRD §2 item 2 lists "confidence-aware candidate extraction" among the intended
  contributions, and PRD §12 says the controller must use confidence to decide how aggressively to
  trust predictions.
* **Actual (verified, `git grep` over `cpp/include` and `cpp/apps` on `e6dde1c`):** `confidence`
  is declared on `Candidate` (`candidate_manager.hpp:108`), copied into `PredictionEvent`
  (`prediction_event.hpp:59,93`) and produced by the predictors. No code in `lpq/`,
  `verification/`, `adaptation/` or the apps reads it to decide control flow. Whether it should
  gate extraction, and whether that helps, is untested.
* **Tested (2026-10-13, candidate branch `completion/m5-candidate`): confidence does not gate
  extraction, and on the LPQ path it is not even populated.** `git grep` over `cpp/include` and
  `cpp/apps`: the only code that reads a predictor's `confidence` is the predictors' own
  `uncertainty_from_confidence(p.confidence)` and the calibration bookkeeping; no code in `lpq/`,
  `verification/`, `adaptation/` or the apps branches on it. `CandidateBetter` orders by `score`
  then node id. The two sites that build a `Candidate` from a queue entry
  (`learned_priority_queue.hpp`, the bucket scan and `refill`) copy `node`, `key` and `score` and
  leave `confidence` at its default `0.0F`, because the bucket substrate stores no confidence.
  `PredictionEvent::from_verification` takes a confidence argument but has no production caller. So
  the "calibrated probability" in the field's comment is never produced on this path.
* **What the adaptive controller does use instead.** `AdaptiveController` (`adaptive_controller.hpp`)
  decides from what verification observed after the fact: it records each outcome's
  `verification.rank_displacement` (line 203) and exposes the mean and 95th-percentile rank error.
  That is feedback from ground truth, not a predicted confidence, so PRD §12 ("The controller
  **must** use confidence to decide how aggressively to trust predictions") is **not met** by it.
  Whether rank-displacement control is an adequate substitute is a design question for the owner,
  not something this register decides.
* **The two tests are characterisation tests, not evidence of compliance.**
  `CandidateManager.ConfidenceDoesNotChangeTheWindowItsOrderOrItsAccounting` and
  `CandidateManager.ALowConfidenceCandidateStillBeatsAConfidentOneWithALowerScore`
  (`cpp/tests/lpq/test_candidate_manager.cpp`) pin what the window does today: offering one
  `(node, key, score)` stream with confidence set to none, certain, inverted against the score and
  seeded-arbitrary gives the same window order and rejection count. They establish that the feature
  is absent; they do not and cannot show that confidence-aware extraction works. They fail if
  `CandidateBetter` is made to order by confidence (mutation checked), and the controller and
  verification code are covered by the `git grep` above, not by a test. No test establishes the
  intended feature, because the feature does not exist.
* **Status:** the claim "confidence-aware candidate extraction" (PRD §2) and the PRD §12 controller
  requirement are **not implemented**. PRD §21 item 3 ("Learned prediction + confidence") and, through
  PRD §25 criterion 11, the ablation of every major component therefore cannot be met until a
  confidence-using variant exists or the owner descopes it
  (`docs/experiments/prd-section-21-status.md`). Not built in this milestone, by decision. If it is
  ever wired in, the two tests above must be changed on purpose.

### D-15. Model artifacts do not embed the provenance specdoc §30 asks for

* **Claimed:** specdoc §30 requires metadata linking a model artifact to its feature schema,
  training dataset, training queries, evaluation queries, hyperparameters, seed, metrics and
  checksum.
* **Actual (verified by reading `python/lpq_tools/training/model_io.py` and `train.py`; v2 does
  not change the `training/` package):** `write_linear_model` writes weights, bias, temperature,
  the feature-layout version and the online hyperparameters. The training report carries the
  seed, graph checksum and query seed, but it is a separate object, not part of the artifact.
* **Status:** recorded; fix deferred to M10.

### D-16. A reported `starlette` / `sse-starlette` pip conflict is not a repository defect

* **Reported:** a handoff note and the first version of `docs/project/PROJECT_STATUS.md` listed a
  conflict in the project's API requirements.
* **Actual (verified):** the string `sse-starlette` appears in no repository file on `main` or on
  `e6dde1c`. In the audit sandbox `pip show sse-starlette` reports it is required by the `mcp`
  package, which is not a project dependency. A `pip install --dry-run` of
  `python/requirements-dev.txt` and `atlas/api/requirements.txt` on `e6dde1c` would install
  nothing, and the CI `engine` job installs them and passes (run #27 and run #28).
* **Status:** closed as not a repository defect. **Inferred**, because it was not reproduced in
  a clean virtual environment; that confirmation is a task for M10.

## Findings added by M3 (2026-10-13)

### D-17. The configured epsilon is not recorded in approximate-mode benchmark results

* **Expected:** an approximate-mode result states the epsilon it was run with, so the file can be
  reproduced and its bound checked later.
* **Actual (verified on `completion/m3-osm-acquisition`):** `lpq_benchmark --mode approximate
  --epsilon 0.1` wrote 1000 records whose `correctness` block holds `path_cost`, `optimal_cost`,
  `approximation_ratio`, `exact_match`, `target_reachable` and `termination`; no field anywhere
  in the file holds the epsilon, and `experiment_id` is `adhoc`. The runner does enforce the bound
  while running: it aborts when `ratio > 1 + epsilon` (`lpq_benchmark.cpp` lines 882 to 886), and
  the run exited 0. So the run is checked, but a stored file cannot say which epsilon it was
  checked against.
* **Status:** **fixed on the candidate branch `completion/m5-candidate` (2026-10-13).**
  `ResultRecord` has an optional `epsilon`, written as `correctness.epsilon` when, and only when,
  the record's mode is `approximate` (including the record for a queue that has no approximation
  mechanism, whose `supported` is false). Exact and adaptive records are byte-identical to before:
  no key, not a null and not a zero. `schema_version` stays 1 because the change is additive and
  optional; a reader that ignores unknown keys, which includes `lpq_tools.evaluation.results`,
  is unaffected, and files written earlier simply lack the key. Tests:
  `ResultRecord.ApproximateRecordsCarryTheConfiguredEpsilonExactly` and
  `ResultRecord.RecordsWithoutAnEpsilonSerialiseExactlyAsBefore` (`cpp/tests/metrics/`), and
  `python/tests/test_approximate_epsilon.py`, which runs the real `lpq_benchmark`. Files written
  before the fix still cannot say which epsilon they ran against. The online-learning record
  (`learner`) is unchanged.

### D-18. Dataset-pipeline hazards found while re-sourcing Monaco

* **`prepare_datasets.py` replaces the tracked `index.json`.** It rewrites
  `datasets/metadata/index.json` from only the datasets it was asked to prepare
  (`docs/local-setup.md` calls the resulting git diff "expected"). Preparing one dataset therefore
  drops the other entries from a tracked file. M3 avoided it by calling `prepare_dataset()` through
  an uncommitted driver, as `scripts/demo/prepare_demo.py` does, and updating only the Monaco entry;
  the four other entries were verified byte-identical. **Fixed in M4** (commit `41906d9`):
  `pipeline.update_index()` merges prepared entries into the existing index, refuses to overwrite one
  it cannot parse, and `prepare_datasets.py` uses it. 13 new tests in
  `python/tests/test_index_preservation.py` failed before the change (the command-line ones with
  `{'b'}` where `{'a','b','c'}` was expected). Run for real with
  `prepare_datasets.py dimacs_ny snap_facebook`, no entry was dropped, all five existing entries
  were identical, and `snap_facebook` (prepared but never indexed) was added; that generated change
  was inspected and then restored, not committed.
* **A stale lock file.** `datasets/metadata/osm_monaco.checksums.json` records the old Geofabrik
  PBF digest. The pipeline writes that lock file only when a config has no pin, so it is not rewritten
  now that the config is pinned. It is left unchanged as a historical record and now describes a file
  the config no longer names. **Investigated in M4 and deliberately not changed.** The lock is a
  first-observation record for files without a pin: it is read only for unpinned files, keyed by
  raw filename, and not rewritten when a file is pinned (`docs/experiments/datasets.md` says
  "Pinning it afterwards is a one-line edit to the config"). `osm_liechtenstein.checksums.json` is
  in the same state. Two tests in `python/tests/test_acquire.py` show a leftover entry for another
  filename is inert for a pinned file and is kept, not replaced, when a new unpinned file is
  recorded; `datasets.md` now states the semantics.
* **A hard-coded raw filename in a test.** `python/tests/test_osm.py` names the raw PBF literally, so a
  re-pin that changes the local filename needs a test edit (done once in M3). **Fixed in M4** (commit
  `099d628`): the path is derived from `configs/datasets/osm_monaco.yaml`. The Monaco test still skips
  when the PBF is absent, and runs when it is present.
* **Status:** fixed in M4 on the local branch `completion/m4-reproducibility-tooling`; the other
  branches are unchanged. Not changed: the lock files.

## Findings added by M4 (2026-10-13)

### D-19. The documented `snap_facebook` model metrics come from a different training protocol

*Revised in M5-P1 (2026-10-13). The M4 entry recorded the non-reproduction and called the cause
"inferred"; it is now verified. The M4 entry also named `findings.md` at the wrong path; it is
`experiments/paper/findings.md`.*

* **Claimed (documented):** `docs/experiments/static-scorer-training.md` gave, for `snap_facebook`,
  held-out ROC AUC 0.7909, in-sample 0.7917 and precision lift 22.25x, and for `dimacs_ny` 0.5331 /
  0.5351 / 1.12x. `experiments/paper/findings.md` (lines 168 to 169, which mixes the Facebook
  pre-correction value with the New York disjoint value 0.5337), `docs/atlas/ABSTRACT.md` (0.79) and
  `docs/experiments/README.md` (line 44) repeat them.
* **Verified:** training on the **benchmark** queries (seed 42: 1,000 queries on `snap_facebook`, 100 on
  `dimacs_ny`), with the same split and seed (25% of nodes held out, `default_rng(20260923)`, logistic
  regression `C=1.0`), reproduces every documented figure exactly: Facebook 0.7909 / 0.7917 / 22.25x with
  4,039,000 observations and 65,079 positives, New York 0.5331 / 0.5351 / 1.12x with 26,434,600
  observations and 2,781,317 positives. The end-to-end counts reproduce exactly too (798,334,612 control and
  803,764,660 model on Facebook; 177,580,603 and 177,732,559 on New York).
  `scripts/reproduce/run_main_experiments.sh` instead trains on the separate seed-917 sets (200 queries):
  Facebook 0.8037 / 0.7909 / 23.08x (807,800 observations, 13,012 positives), New York 0.5337 / 0.5349 /
  1.12x. So the two sets of numbers are two **protocols**, not a defect in either run. The disjoint-protocol
  end-to-end counts are new: Facebook 804,286,113 (+0.745% over the control), New York 177,732,303 (+0.085%).
* **Why the documents lag:** `ablation-waterfall.md` records that the training labels were moved off the
  benchmark queries after ESD.md section 30 was reopened; `static-scorer-training.md` and the Facebook
  figures in `findings.md` were not updated, and that document's own reproduction command still names the
  benchmark manifest.
* **Three protocols (revised after owner review, 2026-10-13).** A: pre-correction (benchmark queries, seed 42).
  B: pair-disjoint (seed 917, no shared (source, target) pair), which is what `run_main_experiments.sh`
  trained on until M5-P1. C: **source-disjoint**, the corrected protocol. B is **not** source-disjoint: the
  collector labels from a full single-source search per source and the target plays no part, and the
  `snap_facebook` B set shares **45 source nodes** with the 1,000-query benchmark manifest (4 with
  `first100`, 1 with `first30`). `dimacs_ny`'s B set shares none with the 100-query and `first30`
  manifests, so B is source-disjoint for those, but it shares 2 sources with the 1,000-query
  `dimacs_ny_queries.yaml`, which no number here uses.
* **Protocol C for `snap_facebook`:** `scripts/data/generate_training_queries.py` (new, tested) wrote
  `snap_facebook_training_queries_source_disjoint.yaml`, seed 917, 200 queries (194 distinct sources),
  drawn from the 3,152 of 4,039 nodes that no benchmark manifest uses as a source (887 excluded). Same
  split, seed and model as A and B. Held-out ROC AUC **0.7985**, in-sample 0.7890, lift **22.91x**;
  end-to-end 804,305,590 comparisons against the 798,334,612 control (**+0.748%**, same manifest, same queue
  configuration, all 1,000 records exact). Training set, model and report are byte-identical on a second
  run. The pool is not small, so the experiment is about the size of B and no separation was weakened.
  Held-out accuracy (0.9836) is slightly below the always-"no" baseline (0.9839).
* **What is not claimed:** that C differs from B because of the overlap. They differ in the queries drawn
  as well as in the exclusion, and one split and one seed per protocol cannot separate that from noise
  (held-out AUC 0.7909 A, 0.8037 B, 0.7985 C). B must not be described as source-disjoint. The derived
  Facebook manifests (`derived_fb_*`) and the 1,000-query New York manifest were not re-examined.
* **Status:** **resolved by documentation and one new tested generator in M5-P1**, on the local branch
  `completion/m5a-d19-protocols`. All three protocols are kept, labelled, side by side in
  `static-scorer-training.md` with configuration, split, seeds, library versions and artifact hashes; the
  other documents carry pointers and their original numbers are unchanged. Classification metrics and queue
  comparison counts are reported separately and no timing is reported.
* **Pipeline (owner-approved follow-up, same milestone):** `scripts/reproduce/run_main_experiments.sh` now
  selects each dataset's training set with `select_training_manifest` (`scripts/reproduce/common.sh`):
  `snap_facebook` trains on the protocol C manifest and `dimacs_ny` on its existing seed-917 manifest, and the
  script stops if a set shares a source with any benchmark manifest it reports against. Tests pin this, and pin
  the hashes of the benchmark manifests and of the protocol B manifests, which are unchanged. The script no
  longer reproduces protocols A and B; their documented results are the historical evidence.
* **Out of scope, reported (owner decision):** the 1,000-query `dimacs_ny_queries.yaml` shares 2 sources with
  the New York training set. It is used for the model-free baseline counts (`lpq-baseline-measurements.md`,
  `datasets.md`) and is a parameter of `test_leakage.py`, which checks pair overlap only. No model result and no
  step of the reproduction pipeline uses it, so it was not changed. The related guard is D-21.

### D-21. The split guard and the Atlas model page are still pair-level and keyed by file name

* **Claimed:** `python/lpq_tools/training/leakage.py` (spec section 31) reports a training/evaluation split as
  `DISJOINT` unless a (source, target) pair repeats; a shared endpoint is "reported, never hidden, but does not
  by itself fail the split", and `python/tests/test_leakage.py` asserts exactly that. The Atlas model page
  (`atlas/api/atlas_api/registry.py`, `split_status`) looks up `<dataset>_training_queries.yaml` and compares it
  with every `<dataset>_queries*.yaml`.
* **Actual (verified by reading both, M5-P1):** that is the pair-level rule that the corrected protocol (C) does
  not accept, and the model page assumes the model was trained on the manifest with that file name. A
  `snap_facebook` model trained on the protocol C manifest is therefore described against the protocol B manifest,
  and the model report does not record which manifest trained it (D-15), so the two cannot be told apart from the
  report (both have seed 917 and 200 queries).
* **Status:** **resolved in M5-D21** on the local branch `completion/m5c-d21-provenance` (based on the D-19 branch,
  which provides the source-disjoint manifest and `shared_sources`).
  * *Guard:* `compare_manifests` and `check_split` take `level` (`pair` or `source`) and always report
    `shared_sources`. With no explicit level, a training manifest that declares `source_disjoint` selection is held
    to source level; every other manifest keeps the legacy pair-level verdict, so no existing workflow changes
    behaviour (the existing tests pass unchanged), but its shared sources are now visible (45 for the pair-disjoint
    Facebook set).
  * *Report:* `train_from_file(..., training_manifest=...)` records `{name, sha256, selection, seed, query_count}`
    after checking the manifest against the training set; `run_main_experiments.sh` passes it. Without it the key is
    `null`; reports from before D-21 lack the key.
  * *Atlas model page:* `_model_info` uses the recorded manifest, accepts it only if its SHA-256 still matches, and
    reports `verified`, `hash_mismatch`, `manifest_missing` or `not_recorded` in `training_manifest_provenance`. A
    mismatch or a missing file gives split status UNKNOWN; nothing is substituted. An older report falls back to the
    old dataset-named file and is labelled `inferred_from_dataset_filename`. The web page shows the provenance, the
    rule used and the shared-source count.
  * *Not done:* the model report still embeds no other provenance (D-15: graph and library versions, model file
    hash); the derived-dataset manifests (`derived_*_training_queries.yaml`) were not examined, no focused check
    having shown a dependency.


### D-20. The order-check results file has no producer in the documented workflow

* **Claimed:** `scripts/reproduce/generate_figures.sh` plots the order-reversed scatter from
  `results/raw/dimacs_ny_order_check_v1.json` and tells the reader to produce it "with the order-check
  experiment".
* **Actual (verified in M4):** running the documented sequence (`build_all.sh`, `run_main_experiments.sh`,
  `generate_tables.sh`, `generate_figures.sh`) produced the ablation file but not the order-check file.
  `generate_figures.sh` exits 0 and prints a warning that the order-reversed scatter is missing and
  that the remaining figure "carries the matrix order that was measured to inflate this effect by 45%".
  The workflow documentation names no command that produces the file.
* **Origin (verified in M5-P2):** the file is read only by `generate_figures.sh`. No script, configuration or
  command in any ref ever produced it (`git log --all -S"order_check"` finds only the commit that added the
  figure script). The ablation configuration differs from the other experiments only by its `pq:` list order,
  so the reversed run was almost certainly an unversioned edit of that list; its output was never committed
  and **cannot be recovered or reconstructed**.
* **Status:** **resolved in M5-P2** on the local branch `completion/m5b-d20-order-check`.
  `configs/experiments/dimacs_ny_order_check_v1.yaml` is the ablation ladder in the opposite order and nothing
  else (a test enforces that); `run_main_experiments.sh` runs it after the ablation and checks its comparison
  counts against the same recorded values; `generate_figures.sh` now requires both results files (present and
  complete) before drawing anything and exits non-zero naming the missing file, with the old warning-and-exit-0
  path removed; `docs/reproducing.md` and a note in `ablation-waterfall.md` document the command and say the
  output is a **new measurement**, not the historical one. The historical 45% figure is unchanged and
  unverified here. Tested: 6 new tests (3 red before the change; the other 3 failed only incidentally), and a full run of the documented sequence.

### Observation, not a discrepancy: timing direction (no performance claim)

In the one reproduction run (1 CPU, unpinned, 11 repetitions, every query flagged noisy) the paired test
reported `binary_heap -> radix_heap` as faster for radix, `radix_heap -> lpq` as slower for LPQ, and
`lpq -> lpq_model` as not distinguishable from zero (CI includes 0, adjusted p 0.37). The comparison counts
matched the recorded ones exactly for all four queues. Timings from this environment are not used for any
claim, per the project's measurement rules.

### D-22. Unsupported rows carry empty identity fields, and a file's summary can read `graph_nodes: 0`

**Observation requiring follow-up; the cause is not diagnosed.** Found on 2026-10-13 while auditing D-17.

* **Observed (candidate branch `completion/m5-candidate`, `lpq_benchmark --experiment` with `pq: [binary_heap,
  lpq]`, `mode: [approximate]`, 5 queries):** the five records for `binary_heap` (`supported: false`,
  "has no approximation mechanism") have `experiment_id: ""` and `graph_nodes: 0`, while the five `lpq` records
  have `experiment_id: "review_eps_cfg"` and `graph_nodes: 19600`. The first record in the file was an
  unsupported one. `atlas_api.analysis.summarise_file` on that file returned `graph_nodes: 0`.
* **Where the values come from (read, not tested):** the unsupported record is built in `lpq_benchmark.cpp` from
  `ResultRecord rec;` with `pq`, `algorithm`, `mode` and the reason set, and no identity fields; measured records
  go through `measure_one`, which sets `experiment_id`, `dataset`, `graph_nodes` and `graph_edges`.
  `analysis.py:148` takes `graph_nodes` from `records[0]`. That is consistent with the observation, but I have
  not confirmed it is the whole story, whether other fields are affected, or whether any consumer relies on
  the empty values.
* **Not checked:** other summary fields that read from `records[0]`, the Atlas pages that show these values, and
  whether older result files behave the same.
* **Status:** open; **no code changed**. Not related to D-17 (the epsilon is present on these rows) and it does
  not affect any recorded comparison count. A follow-up would decide whether unsupported rows should carry the
  run's identity fields or whether the summary should skip them when choosing a file-level value.
