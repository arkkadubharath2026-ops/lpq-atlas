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
| M2 documentation corrections | PASS, pending review | **pending** |
| M3 onward | not started | |

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
