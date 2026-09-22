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

**Prefer graph queries over broad `grep`/file sweeps** when answering questions about
architecture, dependencies, or where something lives:

```bash
graphify query "how does LPQ verification interact with the authoritative queue" --budget 2000
graphify path "Dijkstra" "VerificationEngine"     # how two concepts connect
graphify explain "CandidateManager"               # one concept plus its neighbours
graphify affected "PriorityQueue"                 # blast radius of a change
graphify god-nodes                                # architectural hubs
```

Targeted `Read`/`Grep` on a specific known file is still the right tool. The graph
replaces *exploratory* searching, not precise lookups.

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
