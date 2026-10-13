# Dynamic Approximate Shortest Path Using a Learned Priority Queue

This project builds a high-performance, learning-augmented priority queue for shortest-path algorithms.

The main idea is simple:

**Use machine learning to predict where the next useful priority-queue item is likely to be, but never blindly trust the prediction. Verify it, correct it when necessary, and fall back to a classical priority queue when learning is not helping.**

The project targets research publication, so correctness, reproducibility, fair benchmarking, and honest measurement are as important as raw speed.

## LPQ Atlas: the interactive system

The engine now has a web front end, **LPQ Atlas** (built to [`specdoc.md`](specdoc.md)):
choose an OpenStreetMap, DIMACS or SNAP graph, click a source and destination, run
Dijkstra or A* over any classical heap or the learned queue, and replay the engine's real
execution trace; compare two configurations on the same query; run the benchmark harness
from the browser; inspect models, datasets and stored research results. Everything shown
is produced by the C++ engine.

### Quick start

**Short version** (Linux; Windows is below). Install the prerequisites once, then:

```bash
git clone https://github.com/arkkadubharath2026-ops/lpq-atlas.git
cd lpq-atlas
bash scripts/setup/run.sh
```

Then open **<http://127.0.0.1:3000>**. The first run sets everything up (about 7 minutes on a fast connection, see the
table below), later runs start in about 6 seconds. Stop it with **Ctrl+C**, or from another terminal with
`bash scripts/setup/stop.sh`.

On Windows (PowerShell), from the cloned folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup\run.ps1
# stop: Ctrl+C, or   powershell -ExecutionPolicy Bypass -File scripts\setup\stop.ps1
```

**What `run` does on the first call:** checks the prerequisites and prints the install command for anything missing; creates
`.venv` and installs the Python packages; runs `npm ci`; configures and builds the C++ engine (CMake preset `release`, output
in `build/release/`); converts the built-in synthetic `demo_road` graph (committed, offline, checksum-verified); builds the web
UI; starts the API (port 8000) and the web UI (port 3000); and finally runs an **end-to-end check** that sends a real query
through the web server to the API and the C++ engine and has the engine validate the answer against its reference search. It
prints `smoke test passed` only if that worked. It never needs administrator rights and downloads only pip and npm packages
and GoogleTest (see "Internet access"). The one thing it cannot do for you is install a compiler, CMake, Python or Node:
those are system tools, so install them first (`bash scripts/setup/doctor.sh` lists what is missing and the command to fix it).

#### Supported systems, and how well each is verified

| System | Status |
|---|---|
| Linux, Ubuntu 24.04 (GCC 13.3, CMake 3.28, Ninja 1.11, Python 3.13, Node 22, git 2.43) | **Verified end to end from a fresh clone:** setup, start, a real engine query, stop, repeat setup, all test suites (see `docs/local-setup.md`, "Status of these instructions") |
| Other Linux distributions meeting the prerequisites | expected to work; not tested |
| Windows 10/11, Windows PowerShell 5.1 or PowerShell 7 | scripts provided; CI parses all of them and runs the read-only `doctor` on `windows-latest`. **The engine build, start-up, stop and end-to-end check have not been run on Windows.** If something misbehaves, the manual commands are in `docs/local-setup.md` |
| macOS | **not tested**, not claimed |
| Docker (`docker compose up --build`) | provided; **not run in this verification** |

#### Prerequisites

`git`; a C++20 compiler (GCC 11 or newer, Clang 14 or newer; on Windows MinGW-w64 GCC 12 or newer, or the Visual Studio 2022
Build Tools); CMake 3.25 or newer; Ninja; Python 3.11 to 3.13 (**not 3.14**: the pinned numpy/scipy/pandas have no wheels);
Node.js 20 or newer (22 LTS recommended) with npm. Ubuntu/Debian:

```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build git curl python3 python3-venv python3-pip
# Node 22: https://nodejs.org (apt's nodejs is usually too old)
bash scripts/setup/doctor.sh        # checks everything, changes nothing
```

Windows: install each with `winget install <id>` for `Git.Git`, `Python.Python.3.13`, `OpenJS.NodeJS.LTS`, `Kitware.CMake` and
`Ninja-build.Ninja`, plus a compiler (`docs/local-setup.md` has the options), then run
`powershell -ExecutionPolicy Bypass -File scripts\setup\doctor.ps1`.

#### Check that it works

* The terminal shows five `[ ok ]` lines and `smoke test passed: UI -> API -> engine -> validated result`.
* In the browser: **Route Explorer**, choose the dataset `demo_road`, click two points on the map (or type two node numbers),
  press **Run**. You get a route, the engine's metrics and a replayable search trace. Choosing a dataset that has not been
  downloaded shows its state (`NOT DOWNLOADED`) and never silently falls back to the demo graph.
* Any time while it runs: `.venv/bin/python scripts/setup/smoke.py` (Windows: `.venv\Scripts\python.exe scripts\setup\smoke.py`).

#### Troubleshooting (the common failures; the full table is in `docs/local-setup.md`)

| You see | Do |
|---|---|
| `doctor` says a tool is missing or too old | run the install command it prints, then re-run |
| `port 8000 is already in use` | `ATLAS_API_PORT=8001 ATLAS_WEB_PORT=3001 bash scripts/setup/run.sh` (or put them in `.env`, see `.env.example`) |
| setup fails fetching GoogleTest (no GitHub access) | `bash scripts/setup/setup.sh --no-tests` (skips the C++ tests) or install GoogleTest and set `LPQ_USE_SYSTEM_GTEST=1` |
| `pip install` fails building numpy/scipy | you are on Python 3.14; install 3.13 and delete `.venv` |
| a step failed half way | just re-run `bash scripts/setup/run.sh`; every step is idempotent and resumes |
| the page loads but says the API is unreachable | read `results/atlas/launcher/api.log`; start with `run.sh`, which keeps the UI and API ports in step |
| Windows: "running scripts is disabled" | use the `-ExecutionPolicy Bypass -File` form shown above |

#### Reset generated state (safe)

Nothing below touches `datasets/`, `models/`, `results/raw/`, `artifacts/` or your source files.

```bash
rm -rf results/atlas                # runs, comparisons and benchmark jobs started from the UI, caches, logs (regenerated on use)
rm -rf .venv atlas/web/node_modules atlas/web/.next build/release    # dependencies and builds; the next run rebuilds them
```

(PowerShell: `Remove-Item -Recurse -Force results\atlas` and likewise.) Docker volumes are never created or removed by these scripts.

#### Run the tests

```bash
bash scripts/setup/test.sh                 # everything: C++ (ctest), Python, API, web lint/typecheck/unit
bash scripts/setup/test.sh api web         # or any of: cpp python api web
```

On the verified Linux machine this passes 964 C++ tests, 424 Python tests (12 skipped: they need datasets you have not
downloaded), 117 API tests (1 skipped) and 55 web tests, in about 2 minutes. The browser end-to-end suite
(`cd atlas/web && npx playwright install chromium && npx playwright test`) is separate and is run in CI.

#### Real datasets, OSM and trained models (optional, need the internet)

The demo needs none of them. Real datasets are acquired by a separate, checksum-verified command and are never downloaded
during setup:

```bash
.venv/bin/python scripts/data/prepare_datasets.py --list          # everything declared
.venv/bin/python scripts/data/prepare_datasets.py dimacs_ny       # ~6 MB download, 264,346 nodes (also snap_facebook, ...)
```

* **OpenStreetMap:** `osm_monaco` is pinned to one day's bytes of a file its publisher replaces daily, so a fresh download is
  rejected by the checksum unless the upstream file is unchanged; reproducing it needs the retained file placed in `datasets/raw/`
  and `--skip-download` (discrepancy D-01 in `docs/atlas/DISCREPANCIES.md`). `osm_liechtenstein` names a snapshot that no longer
  exists. **So real OSM data cannot be acquired reliably from a fresh clone today**; this is documented, not hidden.
* **Trained models** (`models/exported/`) are not in git. They are produced from `dimacs_ny` and `snap_facebook` by
  `bash scripts/reproduce/run_main_experiments.sh` (about 11 minutes, offline once those datasets are prepared). Without them the
  `lpq_model` queue says that no model is available for the dataset; everything else works.

#### Research benchmarks run separately from the app

```bash
bash scripts/demo/reproduce_benchmark.sh        # latency comparison on this machine; raw results go to a new file under results/raw/local/
bash scripts/reproduce/run_main_experiments.sh  # the registered experiments (needs the real datasets); see docs/reproducing.md
```

The app and the benchmarks do not share timing code: benchmark timings are taken by `lpq_benchmark` on one pinned core, never by
the web server. Timings depend on the machine and none are claimed here; see [docs/experiments/](docs/experiments/README.md).

#### Time, disk and internet

| | Measured on the verified Linux machine |
|---|---|
| First `run.sh` from a fresh clone (cold pip and npm caches) | 7 min 5 s, to "running" |
| Re-running `setup.sh` on a set-up clone | 32 s |
| Later `run.sh` | about 6 s |
| Disk after setup | 1.3 GB (`.venv` 568 MB, `node_modules` 617 MB, UI build 42 MB, engine build 47 MB, git history 8 MB) |
| Optional real datasets | `dimacs_ny` about 6 MB to download; the others are larger and were not measured here |

| Needs the internet | When |
|---|---|
| pip and npm packages, and GoogleTest (GitHub, only for the C++ tests) | setup, once |
| Map background tiles (`tile.openstreetmap.org`) | while using the map; offline the tiles are blank and the graph still draws |
| Real datasets | only when you run `prepare_datasets.py` |
| Everything else, including the demo, queries, traces, comparison and the benchmark on `demo_road` | **runs offline** |

An optional `.env` file (copy `.env.example`) changes ports and limits; nothing in it is required and it contains no secrets.

Docker alternative: `docker compose up --build`, then open <http://localhost:3000>.

* **How to use it (start here):** [INSTRUCTION_MANUAL.md](INSTRUCTION_MANUAL.md)
* Technical guide and run instructions: [docs/atlas/README.md](docs/atlas/README.md)
* Engine contract (`lpq_run`, trace schema): [docs/atlas/ENGINE_CONTRACT.md](docs/atlas/ENGINE_CONTRACT.md)
* Discrepancy register and defects found while building: [docs/atlas/DISCREPANCIES.md](docs/atlas/DISCREPANCIES.md)
* Revised abstract: [docs/atlas/ABSTRACT.md](docs/atlas/ABSTRACT.md)

## 1. Read these three files first

You should have:

```text
PRD.md
ESD.md
README.md
```

Read them in this order:

```text
README.md
   ↓
PRD.md
   ↓
ESD.md
```

`PRD.md` explains WHAT the project must achieve.

`ESD.md` explains HOW the software should be engineered.

`README.md` explains HOW to start the implementation.

## 2. Important research warning

Do not describe this project as the first "learned priority queue".

There is already published work on Learning-Augmented Priority Queues, including a NeurIPS 2024 paper by Ziyad Benomar and Christian Coester, which studies learning-augmented priority queues and applies them to Dijkstra.

Our research direction therefore needs to be stronger and more practical:

- online adaptation,
- confidence-aware prediction,
- verification,
- correction,
- adaptive fallback,
- exact and approximate modes,
- graph generalization,
- end-to-end runtime accounting,
- hardware-level analysis,
- strong experimental methodology.

Before writing the final paper, perform a fresh literature review and update the novelty statement based on the latest research.

## 3. What we are building

The system will contain:

```text
Dijkstra / A*
      |
      v
   LPQ
      |
      +---- Learned prediction
      |
      +---- Confidence
      |
      +---- Candidate selection
      |
      +---- Verification
      |
      +---- Correction
      |
      +---- Online learning
      |
      +---- Classical fallback
```

The classical priority queue remains the correctness anchor.

## 4. Recommended development environment

Recommended:

- Linux or WSL2.
- Modern C++ compiler.
- CMake.
- Python 3.11+.
- Git.
- Optional Docker.
- Linux `perf` for hardware profiling.

A GPU is not required for the core system.

The primary benchmark should run on CPU so that ML inference is compared fairly with CPU priority queues.

## 5. First step: give the specifications to your coding agent

Give `PRD.md` and `ESD.md` to Claude Code, Codex, or your preferred coding agent.

Use a prompt like this:

```text
You are implementing a publication-grade research software project.

Read PRD.md and ESD.md completely before changing any files.

Do not simplify the architecture merely because the project is complex.

Do not invent requirements that contradict PRD.md or ESD.md.

First inspect the repository and produce a concise implementation plan.

Then implement the project in the order specified in ESD.md.

The most important rule is correctness before optimization.

Build the classical reference implementation and benchmark infrastructure before implementing LPQ.

Every major implementation step must include tests.

Do not claim performance improvements until real benchmarks have been executed.

Do not fabricate benchmark results.

Do not fabricate ML accuracy.

Do not fabricate publication claims.

When a design choice is not explicitly specified, choose the most robust, maintainable, benchmarkable option and document the decision.

Keep the project runnable after every major milestone.

At the end of each milestone, report:
1. What was implemented.
2. Files changed.
3. Tests added.
4. Tests executed.
5. Results.
6. Remaining work.
```

## 6. Do NOT ask the coding agent to build everything blindly in one shot

This is a research system.

Build it in milestones.

Recommended sequence:

```text
Milestone 1
Repository + CMake + testing

Milestone 2
Graph representation

Milestone 3
Reference Dijkstra

Milestone 4
Classical priority queues

Milestone 5
Differential correctness testing

Milestone 6
Benchmark infrastructure

Milestone 7
A*

Milestone 8
Instrumentation

Milestone 9
ML feature extraction

Milestone 10
Prediction models

Milestone 11
LPQ candidate selection

Milestone 12
Verification/correction

Milestone 13
Exact LPQ

Milestone 14
Online learning

Milestone 15
Adaptive controller

Milestone 16
Approximate mode

Milestone 17
SNAP/OSM pipeline

Milestone 18
Synthetic graphs

Milestone 19
Full experiments

Milestone 20
Statistical analysis

Milestone 21
Hardware profiling

Milestone 22
Ablations

Milestone 23
Reproducibility pipeline

Milestone 24
Paper artifacts
```

## 7. The first thing the coding agent should build

Do not begin with the ML model.

The first working system should be:

```text
Graph
  ↓
Reference Dijkstra
  ↓
Binary Heap
  ↓
Correct shortest path
  ↓
Benchmark
```

Then add:

```text
Pairing Heap
Radix Heap
Fibonacci Heap
```

Only after those are trustworthy should LPQ be added.

Otherwise, if LPQ appears faster, we will not know whether the improvement came from the learned idea or from an unfair baseline.

## 8. How to run the project later

The exact commands should be generated by the implementation, but the final repository should eventually support something similar to:

```bash
cmake --preset release
cmake --build --preset release
```

Run tests:

```bash
ctest --preset release
```

Run a benchmark. `--pq all` measures every queue in one process over one query
set, which is what makes the comparison paired:

```bash
./build/release/bin/lpq_benchmark \
    --algorithm dijkstra \
    --pq all \
    --dataset datasets/processed/dimacs_ny.lpqgraph \
    --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
    --repetitions 11 --warmup 3 --require-pinning \
    --output results/raw/ny.json
```

Run a whole experiment from a manifest. This is the reproducible form: the
matrix of algorithm x queue x mode comes from the file, and the run refuses to
proceed unpinned.

```bash
./build/release/bin/lpq_benchmark \
    --experiment configs/experiments/dimacs_ny_exact_v1.yaml \
    --output results/raw/dimacs_ny_exact_v1.json
```

Run approximate mode. The slack is required and must be positive; the runner
checks every query against `1 + epsilon` using the trusted reference, and aborts
if the guarantee is broken:

```bash
./build/release/bin/lpq_benchmark \
    --algorithm dijkstra --pq lpq \
    --mode approximate --epsilon 0.1 \
    --dataset datasets/processed/dimacs_ny.lpqgraph \
    --queries-file experiments/manifests/dimacs_ny_queries_100.yaml
```

Profile where the queue spends its time. This is a **separate binary**, because
the probes perturb what they measure and must never be in the timed benchmark:

```bash
./build/release/bin/lpq_profile \
    --dataset datasets/processed/dimacs_ny.lpqgraph \
    --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
    --pq lpq --repetitions 3
```

Every flag above is real; `lpq_benchmark --help` is the authority if this list
falls behind.

## 9. How experiments should work

Never manually run random experiments and report the best number.

Experiments should be configuration-driven.

For example:

```text
experiment manifest
        ↓
same graph
        ↓
same queries
        ↓
same hardware
        ↓
all algorithms
        ↓
multiple repetitions
        ↓
raw results
        ↓
statistical analysis
        ↓
plots/tables
```

This is essential for a credible paper.

## 10. What counts as a successful result

A successful result is NOT:

> "The ML model has 95% prediction accuracy."

The important question is:

> "Does using the prediction actually make the complete shortest-path computation faster?"

For example:

```text
Baseline PQ:
queue = 100 ms

LPQ:
feature extraction = 8 ms
ML inference = 5 ms
verification = 10 ms
queue savings = 40 ms

total LPQ = 83 ms
```

That is useful.

But if:

```text
queue savings = 20 ms
ML overhead = 35 ms
```

then LPQ made the system worse.

The benchmark must expose this honestly.

## 11. Correctness rule

For exact mode:

```text
LPQ result
     ==
trusted shortest-path result
```

Always validate this.

For approximate mode:

```text
approximation ratio =
returned path cost / optimal path cost
```

Report the complete distribution.

Never report only the average.

## 12. The most important experiment

Eventually we want a graph like:

```text
Prediction quality
        vs
End-to-end runtime
```

and another like:

```text
ML overhead
        vs
Priority-queue savings
```

These experiments explain whether the central research idea actually works.

## 13. Generalization experiment

Do not train and test only on the same graph.

A stronger experiment is:

```text
Train:
Graph family A

Test:
different graph from A
different graph size
different source distribution
different graph family
OSM graph
synthetic graph
```

If the model survives this, the research story becomes much stronger.

## 14. Distribution-shift experiment

The system should intentionally be exposed to situations where predictions become worse.

Examples:

- different graph topology,
- changed edge weights,
- modified road network,
- different source distribution,
- different graph size.

The important result is not that the model never fails.

The important result is:

**When the model fails, LPQ should detect that and degrade gracefully.**

## 15. Do not optimize for ML accuracy alone

A model with higher prediction accuracy can still be worse.

For example:

```text
Model A
accuracy: 94%
inference: 1 μs

Model B
accuracy: 98%
inference: 30 μs
```

Model A may be much better for the actual system.

The final model should be selected using end-to-end performance.

## 16. Do not remove the classical fallback

The fallback is a major part of the architecture.

Conceptually:

```text
Prediction
    ↓
Confidence
    ↓
Verification
    ↓
Is learned mode beneficial?
   / \
 yes  no
 |     |
LPQ   classical PQ
```

This makes the system robust when the workload changes.

## 17. Keep all raw results

Never overwrite benchmark results.

Use:

```text
results/
├── raw/
├── processed/
├── figures/
└── tables/
```

Raw results should remain available so every figure in the paper can be traced back to actual measurements.

## 18. Git discipline

Commit after meaningful milestones.

Suggested commit progression:

```text
initial architecture
graph representation
reference dijkstra
binary heap
pairing heap
radix heap
fibonacci heap
benchmark framework
astar
instrumentation
feature extractor
prediction engine
lpq candidate manager
verification engine
exact lpq
online learner
adaptive controller
approximate mode
dataset pipeline
experiment suite
analysis pipeline
paper reproduction
```

Do not commit generated datasets or enormous benchmark outputs unless deliberately versioned.

## 19. Before claiming the project is successful

We need evidence for all of these:

```text
[ ] Exact correctness
[ ] Approximation behavior measured
[ ] Classical baselines implemented
[ ] LPQ benchmarked fairly
[ ] ML overhead measured
[ ] Verification overhead measured
[ ] Online learning measured
[ ] Fallback behavior tested
[ ] Synthetic graphs tested
[ ] SNAP tested
[ ] OSM tested
[ ] Unseen graphs tested
[ ] Distribution shift tested
[ ] Ablation study completed
[ ] Statistical analysis completed
[ ] Hardware profiling completed
[ ] Reproduction scripts work
[ ] Literature review updated
```

Only then should we make strong publication claims.

## 20. Final philosophy

The project should follow one rule:

**Do not build a flashy ML demo. Build a serious algorithmic system and use ML only where it demonstrably improves the system.**

If the experiments show that one component does not help, remove it.

If a simpler model beats a neural model, use the simpler model.

If a classical priority queue beats LPQ on a workload, report it.

If LPQ wins only on certain graph families, identify those conditions.

If the original hypothesis is wrong, change the hypothesis rather than forcing the data to support it.

That is how this project can become a credible research contribution rather than just a complicated implementation.

## v2 results (measured, with limits)

What the v2 optimisation work did and did not achieve, with every number traceable to
raw results and its confidence interval: [`docs/experiments/v2-results.md`](docs/experiments/v2-results.md).
In short: `lpq_optimized` (an exact, per-graph autotuned bucket queue) is faster than the
radix heap on DIMACS New York Dijkstra and equal to it elsewhere; a trained scorer and a
learned selection policy did not help. Prior art and novelty limits:
[`docs/research/literature-and-novelty.md`](docs/research/literature-and-novelty.md).
