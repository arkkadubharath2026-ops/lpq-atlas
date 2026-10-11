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

On a fresh `git clone` (Windows or Linux), with no network access needed after the
dependencies are installed. Full prerequisites, per-OS install commands and a
troubleshooting table: **[docs/local-setup.md](docs/local-setup.md)**.

```bash
# Linux / macOS / Git Bash                           # Windows PowerShell (see docs/local-setup.md)
bash scripts/setup/doctor.sh                         # scripts\setup\doctor.ps1
bash scripts/setup/run.sh --test                     # scripts\setup\run.ps1 -Test
bash scripts/demo/reproduce_benchmark.sh             # scripts\demo\reproduce_benchmark.ps1
```

1. `doctor` checks for a C++20 compiler, CMake, Ninja, Python 3.11 to 3.13 and Node 20+,
   and prints the install command for anything missing.
2. `run --test` sets the clone up on first use (virtualenv, `npm ci`, release build,
   the offline synthetic `demo_road` dataset), runs the C++, Python, API and web tests,
   then starts the API on <http://127.0.0.1:8000> and the web UI on <http://127.0.0.1:3000>.
   Stop it with Ctrl+C.
3. `reproduce_benchmark` runs a baseline-versus-alternatives latency comparison
   (`lpq_benchmark --interleave`) on this machine and writes the raw results to a new
   file under `results/raw/local/`. Timings depend on the machine and are not
   claimed here; see [docs/experiments/](docs/experiments/README.md) for recorded results.

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
