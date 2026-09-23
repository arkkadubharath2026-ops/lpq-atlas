# CLAUDE.md — Dynamic Approximate Shortest Path Using a Learned Priority Queue (LPQ)

## Authoritative specifications

`PRD.md` (what to achieve), `ESD.md` (how to engineer it), and `README.md` (how to start)
are the authoritative specification. Do not introduce requirements that contradict them,
and do not simplify the architecture to finish faster.

Core rule: **correctness before optimization**. The classical priority queue is the
correctness anchor; the learned component is only ever a performance hint.

## Never fabricate

Do not report benchmark numbers, model accuracy, approximation ratios, or completed
functionality that was not actually measured or actually implemented. An unmeasured
claim must be labelled as unmeasured.

## Codebase context: Graphify

This repository is indexed by [Graphify](https://graphify.net/), which builds a queryable
knowledge graph of the codebase so architectural questions can be answered without
reading (and paying tokens for) every file.

- Graph data lives in `graphify-out/` (git-ignored — regenerable from source).
- `.graphifyignore` excludes build output, datasets, raw results, and model checkpoints,
  which are large and carry no architectural signal.

**Using the graph is mandatory, not a preference.** Any question about the codebase —
what a component is, what calls it, what it depends on, what a change would affect,
where something lives, or whether a change has landed everywhere it must — is answered
by querying the graph *first*, before reading or grepping:

```bash
graphify query "how does LPQ verification interact with the authoritative queue" --budget 2000
graphify path "Dijkstra" "VerificationEngine"     # how two concepts connect
graphify explain "CandidateManager"               # one concept plus its neighbours
graphify affected "PriorityQueue"                 # blast radius of a change
graphify god-nodes                                # architectural hubs
```

Reading a file directly is still right for the *contents* of a file whose role in the
answer the graph has already established. It is not a substitute for asking the graph a
structural question, and it does not come first.

The carve-out above is narrower than it reads, and it has been stretched before. Knowing
a file's path is not the same as knowing what else reaches into it — which is exactly the
question the graph answers cheaply and a file read does not answer at all. A single
`graphify explain "BucketSubstrate"` established that the component has no incoming
references outside its own header and its own test, and therefore that probes placed in
`LearnedPriorityQueue` capture every path into it. No amount of targeted reading would
have produced that argument.

**If an argument is forming for why this particular task does not need the graph, that
argument is the error, not the exception.**

### Keeping the graph fresh

Rebuild after each milestone, or whenever a batch of files is added or restructured:

```bash
scripts/build/refresh_graph.sh          # code-only, local AST, offline, no API key
scripts/build/refresh_graph.sh --full   # also re-extracts PRD.md/ESD.md/docs (needs an LLM backend)
```

Windows: `scripts/build/refresh_graph.ps1` (add `-Full`).

Code-only extraction uses tree-sitter locally and is deterministic and offline — this is
the default because it never depends on a network service mid-development.

## Repository layout

See `ESD.md` §3. In short: `cpp/` holds the performance-critical engine (graph, algorithms,
priority queues, LPQ, features, models, verification, adaptation, metrics), `python/` holds
training/evaluation/statistics/plotting tooling, `configs/` and `experiments/manifests/`
drive reproducible runs, and `results/raw/` is immutable measurement output.

The shortest-path hot path stays in native C++. Python never enters the timed loop.

## Implementation order

Follow `ESD.md` §39 — classical system and measurement infrastructure first, ML last, so
the learned contribution can be evaluated against a trustworthy baseline.

## Measurement and testing standards

These are standing rules, not per-milestone choices.

**Randomised testing.** Every RNG seed is explicit and preserved, so any run can be
replayed exactly. Every seed that has ever produced a failure is promoted to a permanent
named regression test and kept forever — a seed that found a bug once is evidence that
the bug's shape is reachable, so the fuzz corpus grows monotonically and a failure never
silently stops being covered.

**Benchmarking.** Benchmarks are pinned to a single CPU core. This is not optional on the
development host, an Intel i5-1235U with 2 performance and 8 efficiency cores: without
pinning, the scheduler migrates the benchmark thread between core types mid-run and the
resulting swing has nothing to do with the algorithm under test and can exceed the effect
being measured. Report **median and median absolute deviation**, not only the mean — a
single migration artifact moves a mean substantially while leaving the median intact.
Outliers are never silently dropped.

**Sanitizers.** Sanitizer coverage is never weakened to make a run pass. Disabling
LeakSanitizer with `detect_leaks=0` to silence a teardown crash is specifically
forbidden: it resolves the symptom by discarding the check. Diagnose the root cause. The
genuine fixes found so far were granting the container `SYS_PTRACE` and launching ctest
under `setarch -R`, because WSL2's high ASLR entropy intermittently prevents ASan from
mapping its shadow memory.

**Hardware counters.** Unavailable PMU counters are reported as not measured, never
estimated. WSL2 exposes no `cpu` PMU, so cycles, instructions, branch misses, cache
misses, LLC misses and memory bandwidth cannot be collected on the development host —
`perf` reports `<not supported>` even with `--privileged`. No figure may be inferred,
modelled or extrapolated to fill that gap. `PRD.md` §20 permits documenting unavailable
counters; `docs/experiments/profiling.md` is where that documentation lives, together
with the genuine alternatives (native Linux, or Intel VTune on Windows).

## Working style

**Parallelise.** Fan independent work out rather than serialising it: long verification
runs (the Windows `ctest` suite, the Linux GCC/Clang/sanitizer matrix in Docker, a
Graphify refresh) belong in background tasks, and self-contained writing tasks such as
documentation are good subagent work.

This does not relax the no-fabrication rule. A subagent starts with no context and cannot
verify a number it was not given, so **every measured figure a subagent needs must be
passed to it explicitly**, together with an instruction to use only those figures and to
omit anything it cannot verify from the files it reads.

**Report honestly.** Say plainly when something is not yet wired up, when a claim is a
budget rather than a result, and when a file was accepted on trust rather than verified.
Prefer a stated limitation to an implied capability.
