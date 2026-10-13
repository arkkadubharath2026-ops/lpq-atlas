# Implementation progress log

Durable record of the milestone plan in `LPQ_ATLAS_IMPLEMENTATION_HANDOFF.md` (supplied by the
owner; not tracked in this repository). One entry per milestone, newest last. It is evidence
for the owner's reviews, so each entry says what was run and what was not. It does not replace
`PROJECT_STATUS.md` or `PROJECT_OVERVIEW.md`.

**Evidence labels.** *Verified now*: executed or inspected in the audit sandbox on the date
shown. *CI verified*: a named GitHub Actions run on the exact commit. *Documented, not rerun*:
stated in a project document and not reproduced. *Inferred*: plausible, not checked. *Not tested /
blocked*: not run, with the reason.

**Working branch:** `completion/v2-baseline`, local only, created from `e6dde1c` with no upstream.
Nothing has been pushed, merged or opened as a PR. `main`, `claude/affectionate-gates-53lchk` (v2)
and `claude/sleepy-heisenberg-fhr3me` (docs) are unchanged.

| milestone | status | owner review |
|---|---|---|
| M0 read-only reconnaissance | PASS | approved |
| M1 branch and baseline | PASS | approved |
| M2 documentation corrections | PASS | approved |
| M3 Monaco real-OSM acquisition | PASS for Monaco, with stated limits | accepted (conditional PASS) |
| M4 reproducibility and tooling | PASS for the five scoped items; limits below | **pending** |
| M5-P1 D-19 training protocols (branch `completion/m5a-d19-protocols`) | PASS | **pending** |
| M5-P2 D-20 order-check producer | not started | |
| M5-D21 split guard and model provenance (branch `completion/m5c-d21-provenance`, on the D-19 branch) | PASS | **pending** |
| M5 online learning O1 to O4 (branch `completion/m5-online-o1-o4`, on the D-21 head) | PASS for the stated scope; **O4 checkpoint pending review** | **pending** |
| M5 online learning O5 to O9 | not started | |

---

## Milestone M0: Read-only repository reconnaissance
Status: PASS
Date: 2026-10-13
Base commit SHA: `b2049c2b71d62a7852fef453e30ab59331bb2b7f`
Working branch: `claude/sleepy-heisenberg-fhr3me` (no code change; nothing edited)

### What I found
- Remote heads (GitHub API and `git ls-remote` agree): `main` `4e7ad00`, v2
  `claude/affectionate-gates-53lchk` `e6dde1c`, docs `claude/sleepy-heisenberg-fhr3me` `b2049c2`.
  One PR: [#1](https://github.com/arkkadubharath2026-ops/lpq-atlas/pull/1), open, **draft**, v2 into
  `main`, 21 commits, 165 files. [Verified now]
- Docs commit `b2049c2` adds only `docs/project/PROJECT_OVERVIEW.md` and `PROJECT_STATUS.md`.
  Those do not overlap the 165 files v2 changes. The authoritative specs (`CLAUDE.md`, `PRD.md`,
  `ESD.md`, `specdoc.md`) are byte-identical between `main` and v2. [Verified now]
- [Run #28](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38364982184) on
  `b2049c2` failed. `engine`, `web`, `fuzz` succeeded; `e2e` failed at "Prepare the OSM Monaco
  dataset" with `monaco-260925.osm.pbf: HTTP 404 Not Found`. [CI verified, log read]
- [Run #27](https://github.com/arkkadubharath2026-ops/lpq-atlas/actions/runs/38148343456) on v2
  `e6dde1c` succeeded on all five jobs, with `e2e` on the bundled `demo_road`. [CI verified]
- `OnlineLearner::maybe_update` and `flush` are defined at `online_learner.hpp:246` and `:259` and
  referenced only by `test_online_learner.cpp`, on both branches. [Verified now, `git grep`]
- `confidence` is declared (`candidate_manager.hpp:108`) and copied into `PredictionEvent` but
  read by no control-flow code in `lpq/`, `verification/`, `adaptation/` or the apps.
  [Verified now, `git grep`]
- 20 files under `scripts/` are mode 100644 on v2 (8 `.sh`, 7 `.ps1`, 5 `.py`); 11 are 100755.
  [Verified now, `git ls-tree`]
- v2 does not edit the four documents that state the 34% adaptive claim. [Verified now]
- `sse-starlette` appears in no repository file; in the sandbox it is required by `mcp`.
  [Verified now]
- Geofabrik is unreachable from the sandbox (connection reset by the egress proxy); SNAP and
  DIMACS hosts return HTTP 200 to `HEAD`. [Verified now]
- Claude Code `2.1.296`; no plugins installed. `claude doctor` and `/context` not run.

### Verification actually run
Read-only Git (`status`, `branch -avv`, `log`, `ls-remote`, `merge-base`, `rev-list`, `diff
--name-status`, `git grep`, `git ls-tree`), GitHub API reads (branches, PRs, runs, jobs, job-log
tail), `curl -I` probes. No build, no tests, no file edits.

### Acceptance criteria
Met: starting SHA and tree recorded; branches, PR and CI verified with URLs and SHAs; known gaps
mapped to source; no change made. Partly met: dependency map is coarse. Not met at the time:
owner confirmation of the branch plan (given afterwards).

### Risks and decisions
A green v2 CI is not a real-OSM acceptance. The sandbox cannot reach Geofabrik, so M3 needs either
network access to a durable OSM source or an owner-supplied PBF with its checksum.

---

## Milestone M1: Safe working branch and baseline reproduction
Status: PASS (no failures; see uncertainties)
Date: 2026-10-13, 08:39 to about 09:05 UTC
Base commit SHA: `e6dde1ceca9228e4d2c8bf3c452bab9fd6c72900`
Working branch: `completion/v2-baseline`

### What I found
Baseline for the v2 head, run in a clean detached worktree at the identical SHA so that the
existing `build/` (older `main` binaries), `node_modules`, `.next` and `graphify-out` were not
touched. The API tests hard-code `REPO/build/release/bin` (`atlas/api/tests/conftest.py:20`), so
running them in the primary tree would have tested the wrong engine.

| suite | command | exit | result |
|---|---|---:|---|
| C++ configure | `cmake --preset release` | 0 | ok |
| C++ build | `cmake --build build/release` | 0 | 155 targets, 0 warnings, 4m40s |
| C++ tests | `ctest --test-dir build/release -j 4 --output-on-failure` | 0 | **914/914** (unit 886, differential 24, fuzz-smoke 4) |
| Python | `python -m pytest -q -rs` in `python/` | 0 | **300 passed, 12 skipped** |
| API contract | `python -m pytest -q -rs` in `atlas/api/` | 0 | **87 passed, 1 skipped** |
| Web install | `npm ci` | 0 | 11 audit vulnerabilities reported (1 moderate, 8 high, 2 critical) |
| Web lint / typecheck | `npm run lint`, `npm run typecheck` | 0, 0 | 0 errors, 7 warnings; clean |
| Web unit tests | `npm test` | 0 | **41/41** |
| Web build | `npm run build` | 0 | compiled |
| End-to-end | `prepare_demo.py`, API and `next start`, `npx playwright test` | 0 | **8 passed, 2 skipped** |

Skips, none a failure: 12 Python (snap_facebook not prepared x7; Monaco not acquired; `dimacs_ny`
graph or profile missing x3; one v2 result file absent), 1 API (by design), 2 e2e (needs
`snap_roadnet_pa`; needs stored NY results and model). Playwright used the project-supported
`ATLAS_CHROMIUM_PATH` override; no config was changed. Logs: scratchpad `m1_logs/` (outside the
repository).

### Environment (verified now)
Ubuntu 24.04.5, kernel 6.18.44-fc-v114, x86_64, **1 CPU**. g++ 13.3.0, CMake 3.28.3, Ninja 1.11.1.
Python 3.13.16 (pytest 9.0.2, numpy 2.2.4, scikit-learn 1.6.1, fastapi 0.118.0, starlette 0.48.0,
uvicorn 0.37.0, httpx 0.28.1). Node 22.22.0, npm 10.9.4, next-server 16.3.6.

### Acceptance criteria
Met: one identified SHA; commands, exit codes, counts and logs recorded; nothing in the primary
tree overwritten; the C++ (914) and web (41) counts match the PR text.
Not met / not run: libFuzzer job, Debug ASan/UBSan preset, `ctest --preset release-fast`,
Windows build, benchmarks, reproduction scripts, repeated runs (every count is a single run), real
OSM, any run against real datasets.

### Risks and decisions
The 12 skipped Python tests cover the training and leakage pipeline on real data, OSM extraction
and policy export, so those paths are unexercised by this baseline. The scratch worktree is kept
for now.

---

## Milestone M2: Documentation corrections
Status: PASS, pending owner review
Date: 2026-10-13
Base commit SHA: `e6dde1c`
Working branch: `completion/v2-baseline`
Commits: `57927d9` (docs commit `b2049c2` cherry-picked, identical blobs), `b70c138` (corrections),
and the commit that adds this file. Local only.

### What changed
Documentation only. No source, dependency, workflow, benchmark or PR change.
- `docs/project/PROJECT_OVERVIEW.md`, `PROJECT_STATUS.md`: revision notes; the 34% claim marked
  historical and retracted; the false "v2 supersedes it" statement removed; the dependency-conflict
  claim corrected; CI, test counts and script-mode facts updated; verified versus untested
  separated. Percentages not re-scored.
- `experiments/paper/findings.md`, `docs/atlas/ABSTRACT.md`, `docs/experiments/adaptive-controller.md`,
  `docs/experiments/README.md`: dated retraction notices; the original text is unchanged.
- `docs/atlas/DISCREPANCIES.md`: D-01 and D-02 reopened, D-06 updated (original statuses kept in
  the text), D-11 to D-16 added.
- `docs/project/IMPLEMENTATION_PROGRESS.md`: this file.

### Verification actually run
`git diff --check` (clean); every changed file is `*.md`; no path under `cpp/`, `python/`, `atlas/`,
`scripts/`, `configs/`, `.github/`, `cmake/`, or a requirements or package file; a relative-link check
over the seven edited documents (all resolve once this file exists; the 9 external links, which
are GitHub Actions run URLs, were not fetched, but both run IDs were read through the API);
consistency greps for stale statements and register IDs; the retracted claim appears nowhere
else in the repository, including the UI and API code.

### Acceptance criteria
Met: no outdated result presented as current without a version label; the retraction and v1 text
both stay visible; each statement traces to a command or a named document. Not met: the corrected
text exists only on this local branch, so `main`, v2 and the docs branch still state the 34% claim
un-retracted. Carrying it forward needs a merge or push, which is the owner's decision.

### Proposed replacement for PR #1's "Verification" test counts (not applied)
Current text says Python 311 passed and API 77 passed. The reproduced values on `e6dde1c` are:
"C++ Release 914/914; Python 300 passed, 12 skipped (data not acquired); API 87 passed, 1 skipped;
web 41 unit tests, lint 0 errors, typecheck, production build; Playwright e2e 8 passed, 2 skipped
(on the bundled demo_road)." Single runs, 2026-10-13.

### Unresolved discrepancies after M2
1. The corrections exist only on `completion/v2-baseline` (see above).
2. D-12: the `online_learning` result flag and UI badge read true when the learner is attached,
   though no model weight changes. Not changed (code); due with M5.
3. D-13: 7 documented-as-executable `.sh` scripts are mode 100644. Due with M4.
4. D-14, D-15: `confidence` does not gate extraction; model artifacts lack specdoc §30 provenance.
   Due with M7/M9 and M10.
5. D-16 is inferred: not reproduced in a clean virtual environment.
6. OSM acquisition is broken (D-01); Liechtenstein untested; the sandbox cannot reach Geofabrik.
7. The 34% attribution rests on two figures from `v2-results.md` that were not re-run.
8. The skipped tests and the checks not run, both listed under M1, remain untested.

### Owner decision needed
1. Review the M2 diff (`git diff e6dde1c..HEAD -- docs experiments`).
2. How should the corrections reach the branches that still state the claim? (No action taken.)
3. Push policy, still open; and the OSM source for M3 (network access to a durable source, or a
   supplied PBF with checksum). Proposed next milestone: M3, OSM acquisition.

---

## Milestone M3: Real-OSM acquisition for Monaco
Status: PASS for Monaco, with stated limits (durable retention and Liechtenstein remain open)
Date/time: 2026-10-13, 09:28 to about 09:43 UTC
Base commit SHA: `0981b1acd3a422788a79b94d620dea1302499f56`
Working branch: `completion/m3-osm-acquisition` (local only, in a scratch worktree; no upstream)
Commit: `cf5aea4` (all tracked changes). This log entry is the commit after it.

### What I found
- **Source (verified now).** `download.openstreetmap.fr/extracts/europe/monaco.osm.pbf`, 1,105,253 bytes,
  last modified 2026-10-13 01:16:03 GMT. Headers, the MD5 sidecar and `state.txt` were identical before and
  after the download. The first identity attempt was invalid (the "before" snapshot timed out under
  CPU load); it was archived and the whole sequence redone with longer timeouts.
- **Checksums (verified now).** Publisher MD5 `df804c9f8e6da26ddb350af60132554c` equals the local MD5
  from two tools. SHA-256 `26ac807383921434f6fc7ef07ddb367e03c8357e65ac360c72d5de120c239d1d` from
  `sha256sum`, Python `hashlib`, `openssl` and the repo helper, all equal. Two separate downloads and the
  pipeline's own third download are byte-identical. The MD5 is a corruption check, not authenticity,
  so the pin is trust-on-first-use.
- **Header (verified now).** Replication timestamp `2026-10-13T01:14:05Z` and sequence `7326506` equal
  `state.txt`. No header bbox. 68,533 nodes, 10,168 ways (4,298 highway-tagged), 371 relations. The raw
  nodes span latitude 37.27 to 43.78 and longitude 7.21 to 7.62; none of the far ones enter the road graph.
- **Extracted graph (verified now).** 7,592 nodes, 11,680 arcs, graph checksum 7879006388132312723
  (`0x6d57da90b3a8f693`), bbox [7.4053929, 43.7232244, 7.4472402, 43.7545284], 4,298 ways seen, 1,265
  kept, 619 nodes dropped outside the largest strongly connected component, 32 segments with a missing
  node location. The earlier Geofabrik graph had 5,692 nodes, 8,495 arcs and bbox [7.4053929, 43.7232244,
  7.4423019, 43.7519682]: **a different graph**, about 1.23x the area, same west and south edges.
- **Pre-registered triggers** (written to a file before any bytes were fetched): T1 nodes over 20,000,
  T2 bbox area over 0.004244 square degrees, T3 bbox outside the publisher polygon bbox. **None fired**
  (7,592; 0.001310; inside), so M3 continued. The coverage difference is reported, not hidden.
- **No performance claim anywhere in this milestone** (1 CPU, unpinned, single repetition).

### What changed
Twelve tracked files, one local commit (`cf5aea4`): `configs/datasets/osm_monaco.yaml` (new URL, dated
local filename `monaco-osmfr-20261013T011405Z.osm.pbf`, SHA-256 pin, full provenance, licence);
`datasets/metadata/osm_monaco{,.osm,.conversion}.json` (regenerated); `datasets/metadata/index.json`
(Monaco entry only: four lines; the four other entries byte-identical, checked after a byte-exact round-trip
test); `experiments/manifests/osm_monaco_queries.yaml` (regenerated, seed 42);
`python/tests/test_osm.py` (one line: the raw filename); `docs/atlas/DISCREPANCIES.md` (D-01, D-17, D-18),
`docs/atlas/README.md`, `docs/local-setup.md`, `INSTRUCTION_MANUAL.md`, `docs/project/PROJECT_STATUS.md`.
No CI, C++, web, API, script or dependency change. The PBF and the processed graph are git-ignored and
not tracked or distributed. `osm_monaco.checksums.json` was left unchanged (stale; see D-18).

### Verification actually run (worktree `m3`, all exit 0 unless stated)
| check | result |
|---|---|
| `cmake --preset release`, build, `ctest --test-dir build/release -j 4` | 155 targets, 0 warnings, **914/914** |
| one-off driver, run 1 (retained bytes, no network) | 7,592 nodes, 11,680 arcs, 7 output files hashed |
| one-off driver, run 2 (empty raw dir, pipeline downloads, forced reconvert) | all 7 outputs **byte-identical** to run 1 |
| rejection tests with the repo's `acquire_file` (loopback server) | good bytes accepted; flipped byte rejected on download and left no file; flipped cached file rejected (not repaired in place); old Geofabrik digest rejected |
| `lpq_benchmark` Dijkstra `--pq all`, 1000-query manifest, 1 rep | 6000 records, **all exact against the reference**, nothing unsupported |
| `lpq_benchmark` A* `--heuristic haversine --pq all` | 6000 records, **all exact** |
| `lpq_benchmark` LPQ `--mode approximate --epsilon 0.1` | 1000 records, runner's `ratio > 1 + epsilon` gate did not fire; all ratios 1.0 on this graph |
| old (Geofabrik) manifest against the new graph | **refused**, exit 1: `generated for a graph with 5692 nodes but this graph has 7592` |
| `python -m pytest -q -rs` in `python/` | `test_osm.py` 19 passed; full **301 passed, 11 skipped** |
| `python -m pytest -q -rs` in `atlas/api/` | **87 passed, 1 skipped** |
| web `npm ci`, `lint`, `typecheck`, `npm test`, `npm run build` | lint 0 errors / 7 warnings; unit tests **41/41**; build OK |
| Playwright with `ATLAS_E2E_DATASET=osm_monaco` | **8 passed, 2 skipped**; the 6 job records created are all `osm_monaco` (7,592 nodes), exact |
Evidence files (hashes, headers, logs, JSON results) are in the scratchpad `m3_evidence/`, outside the repository.

### Acceptance criteria (pre-registered)
Met: A1 identity (after the redo), A2, A3, A4, A5, A6, A7 (correctness only), A8, A9, A10, A11, A12.
Partly met: A10 covers the explore, compare and replay flows on real OSM; the unreachable-route and
research/model tests skip (they need `snap_roadnet_pa` and NY results), so spec §43 item 10 is still
untested at the UI level. Not met by design: none. Not in scope: Liechtenstein (deferred by the owner).

### Process notes (what went wrong along the way)
My first identity snapshot was invalid (timeouts) and was redone. A hashing-helper call and a pyosmium
call failed from my own invocation mistakes (a `str` instead of a `Path`; `python3 -I` hid the user-site
package) and were rerun correctly. A benchmark-record check looked for the wrong field name and was
corrected after reading the real schema. Two helper processes (a loopback test server and the web server)
were left running by my scripts; I verified each belonged to my scratch directories, stopped them, and
confirmed the ports were closed.

### Risks and decisions
- **Durability.** The publisher replaces the file daily and keeps no dated copies. The retained bytes exist
  only in the scratch worktree (`datasets/raw/`) and the scratchpad evidence directory, both ephemeral.
  The repository records the SHA-256, so anyone holding the same bytes can reproduce the graph; nobody
  can fetch them from the publisher after the next refresh. Options D1 to D3 from the plan still need
  an owner decision before this is durable.
- **Coverage.** The new graph is larger than the old one; results on the two must not be compared.
- **Licence.** ODbL 1.0, attribution recorded; nothing is redistributed.
- **New findings** (code unchanged): D-17 (epsilon not recorded in approximate-mode results), D-18
  (the `index.json` overwrite hazard, a stale lock file, a hard-coded raw filename in a test).

### Unresolved after M3
1. Durable retention of the Monaco bytes (owner decision).
2. Liechtenstein: still names the dead Geofabrik URL.
3. D-12: the `online_learning` label can read true with no learning; due with M5.
4. D-17, D-18: due with M8/M10 and M4.
5. Research validation gaps unchanged: online learning unwired, ML cost outside the timed clock, approximate
   mode measured on few queries elsewhere, ablations and distribution shift missing, `confidence` does not
   gate extraction (D-14), model provenance (D-15), the 34% attribution not re-run (D-11).
6. The corrected docs and the M3 branch exist only locally; `main`, v2 and the docs branch are unchanged.
7. No real-OSM CI path exists (the owner chose to keep `demo_road` and to defer CI changes).

### Owner decision needed
1. Review the M3 diff: `git diff 0981b1a..completion/m3-osm-acquisition`.
2. Choose how the Monaco source bytes become durable (D1 release asset, D2 commit, D3 re-pin on demand), or accept D4 for now.
3. Approve a proposed M4 (reproducibility baseline) when ready.

---

## Milestone M4: Reproducibility and tooling (local only)
Status: PASS for the five scoped items, with the limits listed below
Date: 2026-10-13
Base commit SHA: `ca9a894abc17a2e7619ac349918f2be802905408` (the accepted M3 commit)
Working branch: `completion/m4-reproducibility-tooling` (local, no upstream; scratch worktree `m4`)
Commits: `ee8d78f`, `41906d9`, `099d628`, `37b7eda`, and the commit that adds this entry.

### What I found
- **D-13 (modes).** Seven scripts the docs run directly were tracked as 100644. Fixed.
- **Index overwrite (D-18).** `prepare_datasets.py` rewrote `datasets/metadata/index.json` from only the
  datasets it was asked about. Fixed; a real run preserved all five entries and added `snap_facebook`.
- **Hard-coded Monaco filename in a test (D-18).** Now read from `configs/datasets/osm_monaco.yaml`.
- **Stale lock files (D-18).** `osm_monaco.checksums.json` (and `osm_liechtenstein.checksums.json`) hold
  an old observation. The lock is read only for unpinned files and keyed by filename, so it is inert once
  a file is pinned. **Left unchanged on purpose**; two tests and a paragraph in `datasets.md` state this.
- **New, recorded only:** D-19 (`snap_facebook` model metrics in the docs do not reproduce) and D-20
  (the order-check results file has no producer in the documented workflow).

### What changed
Starting state (recorded before editing): clean tree, no ignored artifacts, HEAD = `ca9a894`.
Files changed on the M4 branch (excluding this log): the seven scripts' modes; new
`python/tests/test_script_modes.py` and `test_index_preservation.py`; `python/lpq_tools/data/pipeline.py`
(`update_index`); `scripts/data/prepare_datasets.py`; `scripts/demo/prepare_demo.py` (docstring only);
`python/tests/test_osm.py`, `test_acquire.py`; `docs/local-setup.md`, `docs/experiments/datasets.md`,
`docs/atlas/DISCREPANCIES.md`, `docs/project/PROJECT_STATUS.md` (two annotations, not re-scored).
**Not changed:** `.github/`, `cpp/`, `atlas/`, dependency files, the two lock files, generated metadata,
the PBF. Raw and processed data stay untracked.

### Verification actually run (worktree `m4`)
| check | result |
|---|---|
| mode test, before the fix | 7 failed, 7 passed; after: **14 passed** |
| index tests, before the fix | **13 failed**; after: **13 passed** |
| `scripts/reproduce/build_all.sh` | exit 0; 155 targets; ctest **914/914** |
| `prepare_datasets.py dimacs_ny snap_facebook` | exit 0; index kept all 5 entries and added `snap_facebook`; resulting churn inspected, restored, not committed |
| `run_main_experiments.sh` | exit 0; all four comparison counts equal the recorded ones (203,801,439; 71,839,857; 177,580,603; 177,732,303) |
| `generate_tables.sh`, `generate_figures.sh` | exit 0; figures: one of two (see D-20) |
| `scripts/demo/prepare_demo.py` | exit 0; graph checksum `0xebd2ba73ec480d93` equals the committed metadata |
| `python -m pytest -q` in `python/` | **341 passed, 1 skipped** (v2 result file absent) |
| `python -m pytest -q` in `atlas/api/` | **87 passed, 1 skipped** (this `lpq_run` has `lpq_optimized`) |
| web suite, Playwright, Monaco extraction | **not rerun in M4** (no web or engine code changed); Monaco extraction test skips without the PBF in this worktree |
Graph checksums for `dimacs_ny` (`0xd268213e3a5c03e9`) and `snap_facebook` (`0x0061dc4fa16f3a09`) match the documented ones.
Evidence files are in the scratchpad `m4_evidence/`, outside the repository.

### Acceptance criteria
Met: items 1 to 5 of the M4 scope. Not met / not claimed: a timing result of any kind (1 CPU, unpinned,
every query flagged noisy), accuracy of either model, any statement that the whole reproduction matches
the documents (D-19 and D-20 are open).

### Datasets not tested, and why
`osm_monaco` (PBF not present in this worktree; its extraction test skips), `osm_liechtenstein` (dead
Geofabrik URL, deferred by the owner), `snap_roadnet_pa` and other large SNAP/DIMACS sets (not part of
the documented main workflow, not downloaded).

### Remaining reproducibility limits
D-19 and D-20; the Monaco bytes are not durable (D4 from M3); timing cannot be reproduced reliably on one
unpinned CPU; no hardware counters (no PMU); the fix for modes and the index exists only on the local
branch; `models/policy/queue_policy_v1.*` are tracked (from v2) and were not produced by M4;
the retraction of the 34% claim is unchanged (D-11).

### Owner decision needed
1. Review `git diff ca9a894..completion/m4-reproducibility-tooling`.
2. Decide which `snap_facebook` figures are authoritative (D-19), and whether D-20 gets a producer script.
3. Approve M5 (wiring online learning into a real search, D-12) when ready.

---

## Milestone M5-P1: D-19, the training protocols (documentation, one tested generator, re-measurement)
Status: PASS, conditionally approved; revised twice after owner review (three protocols, then the pipeline switched to protocol C)
Date: 2026-10-13
Base commit SHA: `51a8b4cdc7b1e4d486ba40497d6abcaa54ca4e0c` (the accepted M4 commit)
Working branch: `completion/m5a-d19-protocols` (local, no upstream; scratch worktree `m5a`)

### What I found
- Training on the benchmark queries (protocol A, seed 42) reproduces every figure in
  `static-scorer-training.md` exactly, for both datasets, and the Facebook comparison counts too (798,334,612
  control, 803,764,660 model). D-19 is therefore verified. [Verified now]
- Protocol B (seed 917, no shared pair) gives Facebook 0.8037 / 23.08x and 804,286,113 comparisons. It shares
  45 source nodes with the benchmark, and the collector labels from a full single-source search, so B is not
  source-disjoint. [Verified now]
- Owner decision: source-level disjointness. Protocol C, a source-disjoint Facebook training manifest, was
  generated: Facebook held-out AUC 0.7985, lift 22.91x, **804,305,590** comparisons (+0.748% over the same
  control). New York: B shares no source with the 100-query and `first30` manifests, so B is source-disjoint
  there; it shares 2 sources with the 1,000-query manifest, which no number here uses. [Verified now]
- `findings.md` mixes protocols in one table (Facebook A, New York B). [Verified now]

### What changed
Code (new, tested): `python/lpq_tools/data/queries.py` (`exclude_sources` option, `selection` field,
`notes` argument, `shared_sources`; default stream and default output bytes unchanged, pinned by a test),
`scripts/data/generate_training_queries.py` (executable; imports moved into `main` so the ruff findings on the new
file are gone without a lint configuration), `scripts/reproduce/common.sh` (`select_training_manifest`,
`benchmark_manifests_for`) and `scripts/reproduce/run_main_experiments.sh` (trains on the verified source-disjoint
sets: `snap_facebook` on protocol C, `dimacs_ny` on its existing seed-917 set, and stops if a set shares a source with
a benchmark manifest). 28 new tests (`test_queries.py`, `test_training_query_script.py`,
`test_canonical_training_sets.py`), each red before its change. Data: `experiments/manifests/snap_facebook_training_queries_source_disjoint.yaml`.
Documentation: `static-scorer-training.md` (three protocols, tables, commands, hashes), `findings.md`,
`docs/experiments/README.md`, `docs/reproducing.md`, `ABSTRACT.md`, `PROJECT_STATUS.md`, `PROJECT_OVERVIEW.md`,
`DISCREPANCIES.md` (D-19 updated, D-21 new), this log. The benchmark manifests and the protocol B manifest are
unchanged (hashes pinned by a test); C++, CI and dependencies are unchanged.

### Verification actually run (worktree `m5a`; binaries copied from `m4`, byte-identical, no C++ change since M3)
| command | exit | result |
|---|---|---|
| `lpq_benchmark ... snap_facebook ... --pq lpq` (control), twice | 0, 0 | 798,334,612 both times |
| `... --pq lpq_model --model` A / B / C | 0, 0, 0 | 803,764,660 / 804,286,113 / 804,305,590 |
| the three equivalents on `dimacs_ny` (100-query manifest) | 0, 0, 0 | 177,580,603 / 177,732,559 / 177,732,303 |
| `generate_training_queries.py --benchmark ... --seed 917 --count 200` | 0 | 887 excluded, 3,152 allowed, 194 distinct sources |
| `lpq_collect` on the C manifest, then `train_from_file(seed=20260923)` | 0, 0 | recollect and retrain byte-identical |
| `scripts/reproduce/run_main_experiments.sh` (after moving the protocol B models to `m5a_evidence/historical_B_models/`, SHA256SUMS recorded) | 0 | selected `dimacs_ny_training_queries.yaml` (source-disjoint from 2 benchmark manifests) and `snap_facebook_training_queries_source_disjoint.yaml` (from 3); the four NY ablation counts matched (203,801,439 / 71,839,857 / 177,580,603 / 177,732,303); the Facebook model, training set and report are **byte-identical** to the protocol C artifacts, and the NY model to the B/C one |
| `lpq_benchmark` Facebook control and `--pq lpq_model --model models/exported/snap_facebook_static.lpqmodel`, 1 rep | 0, 0 | 798,334,612 and **804,305,590** (+0.748%), pops 3,534,412 and 3,531,802, all 1,000 records exact |
| `scripts/reproduce/generate_tables.sh` | 0 | training report shows held-out AUC 0.798469, lift 22.91x |
| `python -m pytest -q` in `python/` | 0 | **369 passed, 2 skipped** (v2 result file absent; the Monaco extraction test skips because this worktree has no PBF); the 26 source-exclusion, canonical-set and selection tests pass |
| `python -m pytest -q` in `atlas/api/` | 0 | **87 passed, 1 skipped** |
Every benchmark record was exact against the reference. Overlap checks (pairs and sources, all Facebook and New York
manifests) are in the scratchpad `m5a_evidence/`. Python 3.13.16, scikit-learn 1.6.1, numpy 2.2.4, scipy 1.15.2.

### Not done, by design
No timing is reported or claimed, and the counts above are not a performance improvement: the protocol C model does
**more** comparisons than the control (804,305,590 against 798,334,612). The `snap_facebook` wall-clock effect remains
unmeasured. Model files are untracked. The 1,000-query New York manifest is out of scope (D-19: model-free baseline counts
and a pair-level test only). D-21 records that the split guard and the Atlas model page are still pair-level and keyed by
file name.

### Owner decision needed
1. Review `git diff 51a8b4c..completion/m5a-d19-protocols`.
2. D-21: make `lpq_tools/training/leakage.py` and the Atlas model page source-level, and record the training manifest name and
   SHA-256 in the model report?

---

## Milestone M5-D21: source-level split guard and recorded model provenance
Status: PASS
Date: 2026-10-13
Base commit SHA: `3865889` (head of `completion/m5a-d19-protocols`, which supplies the source-disjoint manifest and `shared_sources`)
Working branch: `completion/m5c-d21-provenance` (local, no upstream; scratch worktree `m5c`)

### What I found
- The existing guard treats a shared source as informational and the Atlas model page looked the training manifest up as
  `<dataset>_training_queries.yaml`, so a model trained on the protocol C set would have been described against the
  protocol B set (D-21). The model report could not tell them apart: both have seed 917 and 200 queries. [Verified in P1]
- The derived-dataset manifests were not examined: no focused check showed a dependency on this change.

### What changed (small, additive)
`python/lpq_tools/training/leakage.py` (`level` pair|source, `shared_sources`; automatic: a manifest declaring
`source_disjoint` selection is held to source level, every other manifest keeps the legacy pair-level verdict),
`python/lpq_tools/training/train.py` (`describe_training_manifest`, `training_manifest=` argument, report field
`training_manifest` {name, sha256, selection, seed, query_count}, validated against the training set before it is recorded),
`scripts/reproduce/run_main_experiments.sh` (passes the manifest it selected), `atlas/api/atlas_api/registry.py` (uses the
recorded manifest, verifies its SHA-256, reports `training_manifest_provenance`; older reports fall back to the old inference
and are labelled), `atlas/web/lib/types.ts` and `atlas/web/app/models/[id]/page.tsx` (provenance text, shared-sources and rule
columns, UNKNOWN shown as a warning), `docs/atlas/DISCREPANCIES.md` (D-21), `docs/experiments/static-scorer-training.md`,
`docs/reproducing.md`, this log. New tests: 12 in `test_leakage.py`, 7 in `test_train_provenance.py`, 7 in
`atlas/api/tests/test_model_provenance.py`, written before the code (collection errors or failures first). No C++, CI,
dependency, historical result or manifest change.

### Verification actually run (worktree `m5c`; binaries copied from `m4`, byte-identical)
| command | exit | result |
|---|---|---|
| `scripts/reproduce/run_main_experiments.sh` | 0 | 5 min 41 s; the four NY ablation counts match (203,801,439 / 71,839,857 / 177,580,603 / 177,732,303); the Facebook model and training set are byte-identical to the protocol C artifacts (`f5413deb…`, `b3261e0b…`) and the NY model to the earlier one; the new reports differ from the earlier ones only by the `training_manifest` key |
| recorded in the reports | | `snap_facebook`: `snap_facebook_training_queries_source_disjoint.yaml`, sha256 `e0105cbe…`; `dimacs_ny`: `dimacs_ny_training_queries.yaml`, sha256 `65ab749c…` |
| `_model_info` on the real exported models with the real `lpq_model_info` | | Facebook: provenance verified/recorded, DISJOINT at **source** level against all 3 Facebook benchmark manifests (0 shared sources); New York: verified/recorded, DISJOINT at **pair** level, the 1,000-query manifest shows 2 shared sources (legacy rule, now visible) |
| `python -m pytest -q` in `python/` | 0 | **388 passed, 2 skipped** (v2 result file absent; Monaco extraction skips, no PBF here) |
| `python -m pytest -q` in `atlas/api/` | 0 | **94 passed, 1 skipped** (the API registry is a shared interface, so the suite was re-run) |
| `tsc --noEmit`, `eslint` on the two changed web files (node_modules borrowed by a temporary symlink, removed afterwards) | 0, 0 | clean |
The web unit and Playwright suites were not re-run: the web change is two additive type fields and a display row.

### Remaining
Model reports still embed no other provenance (D-15). The legacy fallback for reports without a record is a guess, labelled as such.
The guard's automatic source-level rule applies only to manifests that declare `source_disjoint`; whether to retro-fit the older
manifests is an owner decision.

### Owner decision needed
1. Review `git diff 3865889..completion/m5c-d21-provenance`.

---

## Milestone M5, stages O1 to O4: measured learner status, event provenance, bounded learner, `lpq_online`
Status: PASS for the stated scope; this is the mandatory O4 checkpoint. Stopped before O5.
Date: 2026-10-13
Base commit SHA: `5306e5b13baf2846ed06bc0265f516644268e38e` (head of the D-21 branch, which contains the D-19 commits and the approved M4 base)
Working branch: `completion/m5-online-o1-o4` (local, no upstream; scratch worktree `m5`)
Ancestry: `51a8b4c` (M4) is an ancestor; D-19 (`3865889`) and D-21 (`5306e5b`) are ancestors; D-20 (`f411ea2`) is **not** (it is independent of M5 and was not merged or cherry-picked). The old branch `completion/m5-online-learning` still points at `51a8b4c`, untouched.

### What I found
- The D-12 flag meant "a learner was attached". The learner's events carried empty features and named the accepted node as the "predicted" node, so even a wired learner could not have produced a correct example. [Verified in code]
- A queue observer, however trivial, caps the candidate window at `ExtractionPolicy::window_size` (default 4). A pass-through learner therefore needs an observer whose policy leaves the window uncapped, or `lpq_online` could not equal `lpq_model`. [Verified in code]
- `StaticNodeScorer` computes through `model.predict(x).score`; `LogisticPredictor::predict` takes `detail::dot(weights, x) + bias`, a double-accumulated sequential sum. The on-demand scorer uses the same function on the same vector, which is why bit-equality holds. [Verified by test]

### What changed
C++ (headers, all additive): `graph_features.hpp` (`fill_static_features`, the one definition of a node's static vector), `correction_engine.hpp` (`predicted_node/key`, `extracted_node/key`, `first_choice_was_minimum()`), `learned_priority_queue.hpp` (stamps them on every extraction path that predicts), `prediction_event.hpp` (`FeatureSource`; the accepted-node example; a fallback weighs more only when the first choice was worse), `controller_observer.hpp` (`make_versioned_event`, `LearningObserver`; `ControllerObserver` takes an optional feature source and otherwise behaves as before), `online_learner.hpp` (example counts, rollback to the exact previous parameters, halting, full accounting, `parameters_within`, `parameters_fnv1a64`), `online_node_scorer.hpp`, `online_session.hpp`, `models/model_report.hpp`, `adaptation/online_model_check.hpp`. Engine: `lpq_run.cpp` (the `learner` block for every `lpq` result, the derived `online_learning` flag, the `lpq_online` queue and its flags). Web: `types.ts`, `learner.ts`, `RunPanels.tsx`. Docs: `docs/experiments/online-learning.md` (new), `DISCREPANCIES.md` (D-12, D-02), `experiments/README.md`, `demo-walkthrough.md`, this log. Not changed: `lpq_benchmark.cpp`, the API, CI, dependencies, any manifest, any historical result, `lpq_adaptive` semantics.

### Verification actually run (worktree `m5`; fresh `cmake --preset release`, then incremental)
| command | exit | result |
|---|---|---|
| baseline, before any edit: `test_adaptation test_lpq test_verification test_models test_features test_differential test_trace` | 0 | 48 / 135 / 54 / 123 / 24 / 24 / 5 passed |
| `cmake --build build/release` (everything), then `ctest -j 1` | 0, 0 | 0 warnings; **960 / 960 passed** (914 before; +46 new: 94 in `test_adaptation` against 48) |
| mutation check: scorer returning `-score` | | the 3 `FrozenEquivalence` configurations fail; restored, they pass. A score scaled by 1.0001 fails the bit-equality test only (order is unchanged, so a search cannot see it, as expected) |
| `python -m pytest -q` in `python/` | 0 | **407 passed, 2 skipped** (19 new in `test_online_engine.py`; skips: v2 result file, Monaco PBF) |
| `npx tsc --noEmit`, `eslint` on the changed web files, `vitest run` (borrowed `node_modules`, symlink removed) | 0, 0, 0 | 47 passed (6 new) |
| `o4_equivalence.py` (scratchpad `m5_evidence/`), Facebook, 1,000 queries, protocol C artifact `f5413deb…` with its report (graph and width checked: match) | 0 | `lpq_model`, `lpq_online` disabled and `lpq_online` periodic: **0 queries** where any search fact differs (result, counters, learned-queue metrics); all 3,000 runs exact against the reference; **804,305,590 comparisons in all three**; periodic applied 5,775 updates, changed the weights on 1,000 of 1,000 queries |
| same, New York, 100 queries, artifact `eca2fb80…` | 0 | 0 differences; all exact; **177,732,303 comparisons in all three** |
| `lpq_benchmark` after the rebuild, 1 rep: Facebook `lpq` / `lpq_model`; New York `lpq` / `lpq_model` | 0 ×4 | **798,334,612 / 804,305,590 / 177,580,603 / 177,732,303**, every record exact: the historical counts, unchanged |
| `lpq_benchmark --pq lpq_adaptive --adaptive`, first 100 Facebook queries: old binary (M5 baseline build) vs new | 0, 0 | 78,105,685 comparisons, 361,997 pops, both |

### Acceptance (stage by stage)
- **O1.** Met: the `learner` block exists; `online_learning` is derived from it; `lpq_adaptive --adaptive-learning` now reads attached / events recorded / 0 updates / false; the badge has separate states; legacy results are not trusted. Playwright was not run.
- **O2.** Met: first choice, extracted node and keys on every predicting path (modular and direct); keys decide the label; two examples on a wrong first choice; none when no prediction was made; features equal the scorer's. `lpq_adaptive` keeps its original events.
- **O3.** Met: on-demand scorer, bit-equal when frozen; bounded ring buffer and batch; exact accounting; rollback restores the previous parameters exactly; halt after `max_rollbacks`; the search continues. Not done: a cap on total work per query beyond the existing `max_batch` and buffer capacity.
- **O4.** Met: `lpq_online`, explicit artifact, report checks (graph, width, static columns, finiteness, container), recorded overrides, between-query updates, frozen equivalence at unit and engine level, historical counts preserved, differential correctness (50 random graphs per configuration, explicit seeds).

### Not verified, not done
- Tests were written alongside the code, not red-first; the mutation checks above are the evidence that they can fail.
- No sanitizer, GCC/Clang matrix or Windows run; no cross-host determinism check (equality is asserted within one binary and host).
- The API suite and Playwright were not re-run (no API source changed); the API and UI do not expose `lpq_online` (O7).
- `lpq_benchmark` has no `lpq_online` or per-query snapshot/restore (O6); the CLI equivalence uses `lpq_run`, one process per query. No in-search updates (O5; `--online-apply in_search` is refused).
- The engine records FNV-1a of the artifact, not SHA-256. Session persistence across `lpq_run` invocations does not exist: each invocation is one query. D-15 (model artifacts embed no provenance) stays deferred; the report supplies it.
- No timing, no performance claim, and no measurement of whether any update improves the model or the search.

### Owner decision needed
1. Review `git diff 5306e5b..completion/m5-online-o1-o4` and approve proceeding to O5.
