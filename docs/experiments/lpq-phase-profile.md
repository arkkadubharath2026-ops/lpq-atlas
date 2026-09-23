# Where the learned priority queue's time actually goes

`lpq-baseline-measurements.md` established that the LPQ is ≈4.6× slower than the
binary heap on `dimacs_ny` while performing 12.9% **fewer** comparisons, and
concluded that the deficit is per-extraction overhead rather than comparison
count. That conclusion named no phase. This document names them, by measurement.

The question here is deliberately narrow: **which part of queue work dominates**.
Nothing was optimised, and the algorithm is unchanged — commit `f035db3` remains
the baseline. Every code change behind these numbers is instrumentation.

## The instrument, and its limits

`cpp/include/lpq/metrics/phase_profiler.hpp` accumulates timestamp-counter ticks
into disjoint phases of `push` and `pop`. It is compiled into a separate binary
(`lpq_profile`), never into `lpq_benchmark`, so the project's published timings
carry no probes.

Four limits govern how far these numbers can be pushed, and all four are
consequences of the instrument rather than caveats added for caution:

**No hardware counters.** No PMU is read, because the development host has none
to read — WSL2 exposes no `cpu` PMU (`docs/experiments/profiling.md`). Cache
misses, branch mispredictions, cycles and memory bandwidth are **not measured**
here and no figure below is derived from them.

**Shares, not durations.** Ticks are reported as proportions of a total, never
converted to nanoseconds. Converting would need a calibrated clock frequency,
and on a laptop whose clock scales with load and temperature that calibration
would be a modelled number presented as a measured one. A ratio of two tick
counts taken on the same pinned core needs no calibration.

**The probes are not free, and their cost was measured.** Each phase boundary is
a pair of `__rdtsc` reads. The cost of one pair is calibrated on every run, on
the same pinned core: **35.9 ticks** on the LPQ run, **38.4 ticks** on the binary
heap run. Six pairs sit inside every LPQ pop. That cost is *inside* every figure
below and is never silently subtracted.

**Total instrumentation overhead was measured, not assumed.** The same source is
built twice — `lpq_profile` with probes, `lpq_profile_noinstr` without — and run
over the identical queries:

| queue | instrumented | uninstrumented | inflation |
|---|---:|---:|---:|
| LPQ | 19.223 s | 13.731 s | **1.40×** |
| binary heap | 5.765 s | 3.280 s | 1.76× |

So roughly 29% of the instrumented LPQ run is instrument. Independently, the
probe count times the calibrated pair cost (249,300,384 × 35.9 = 8.95×10⁹ ticks
against a 29.47×10⁹ total) gives 30%. Two estimates from different
directions agreeing to within a percentage point is the main reason the shares
below are trusted at all.

## Protocol

| | |
|---|---|
| Dataset | `dimacs_ny`, 264,346 nodes / 733,846 arcs |
| Queries | `dimacs_ny_queries_100.yaml` — the same 100-query manifest as the baseline timings |
| Repetitions | 3 passes over the query set, after one uncounted warm-up pass |
| Pinning | single core, core 1, enforced (the run refuses to proceed unpinned) |
| Algorithm | Dijkstra, exact mode, single-target |
| LPQ configuration | `ConstantNodeScorer` — no learned model, as in the baseline |
| Host | Intel i5-1235U, Windows, GCC 16.1 Release |

## Measured phase shares — LPQ

Raw output, ticks as measured, probe cost included:

| phase | ticks | calls | % of total | % of pop | ticks/call |
|---|---:|---:|---:|---:|---:|
| push | 2,186,544,880 | 41,671,074 | 7.42% | – | 52.5 |
| **pop (total)** | **27,282,612,187** | **41,525,862** | **92.58%** | – | **657.0** |
| ↳ verification scan | 10,341,730,873 | 41,525,862 | 35.09% | 37.91% | 249.0 |
| ↳ window maintenance | 8,842,381,514 | 41,525,862 | 30.01% | 32.41% | 212.9 |
| ↳ pop (unattributed) | 4,792,157,657 | – | – | 17.56% | 115.4 |
| ↳ remove from bucket | 2,035,373,779 | 41,504,505 | 6.91% | 7.46% | 49.0 |
| ↳ bucket traversal | 1,266,158,292 | 41,525,862 | 4.30% | 4.64% | 30.5 |
| ↳ fallback scan | 4,810,072 | 21,357 | 0.02% | 0.02% | 225.2 |

Three phases have per-call costs at or below the 35.9-tick probe floor —
`bucket traversal` (30.5), `remove from bucket` (49.0) and `push` (52.5). **Their
shares are not trustworthy**, because most or all of what was measured is the
measurement. What can be said is only that each is *small*: a phase cannot be
expensive and register below the probe cost.

Subtracting one probe pair per call gives a probe-corrected estimate. This is a
**derived** figure, not a measured one, and is labelled as such wherever it is
used:

| phase | gross ticks/call | minus one probe pair | share of corrected pop |
|---|---:|---:|---:|
| verification scan | 249.0 | 213.1 | 41.5% |
| window maintenance | 212.9 | 177.0 | 34.5% |
| pop (unattributed) | 115.4 | 115.4 (no probe) | 22.5% |
| remove from bucket | 49.0 | 13.1 | 2.6% |
| bucket traversal | 30.5 | below the floor | ≲1% |

## The finding

**Verification and candidate-window maintenance together account for roughly
three quarters of extraction time** — 76% of the probe-corrected pop cost, and
about 70% of all queue time. Both are LPQ-specific machinery with no counterpart
in a classical heap.

The comparison with the binary heap through the *same* instrument makes the
scale plain:

| | LPQ | binary heap |
|---|---:|---:|
| pop calls | 41,525,862 | 36,278,403 |
| gross ticks per pop | 657.0 | 79.9 |
| probe-corrected ticks per pop | ≈513 | ≈41 |
| uninstrumented wall clock | 13.731 s | 3.280 s |

Per extraction, the LPQ costs about **12× what the binary heap costs**, and it
performs **14% more extractions** on top of that, because lazy deletion leaves
stale entries the binary heap's decrease-key removes. The uninstrumented ratio,
4.19×, independently reproduces the 4.57× the benchmark measured at `f035db3` in
a different binary under a different protocol.

This is consistent with, and sharpens, the baseline conclusion. The LPQ examines
13.2 entries per extraction against the binary heap's 16.9 — it genuinely does
less comparison work — and then spends roughly 390 ticks per extraction deciding
which entry to examine. **The decision costs an order of magnitude more than the
work it saves.** A better model changes what the window contains; it does not
change the cost of building and verifying one.

## Allocation activity: measured, and not the answer

Counted exactly by a replaced global `operator new` in the profiling binary —
this is a hard count, not a timing:

| | LPQ | binary heap |
|---|---:|---:|
| allocations | 900 | 900 |
| deallocations | 900 | 900 |
| bytes | 961,558,800 | 961,558,800 |

Identical, to the byte, between two completely different queues. 900 = 3
repetitions × 100 queries × 3 allocations, each of ~1.07 MB, which is
264,346 nodes × 4 bytes: these are Dijkstra's per-query result vectors, not
queue memory.

**The queue allocates nothing in steady state.** Allocation is not a contributor
to the deficit, and can be removed from the list of suspects — not because it
seems unlikely, but because it was counted.

## What remains unexplained, and what is only a hypothesis

**17.6% of pop time is unattributed** — 115.4 ticks per extraction that no
sub-phase claims. This is the offer-loop scaffolding between the probes, the
metrics increments, the branch structure, and the `bound_for` call. It is
reported as a remainder rather than distributed across neighbouring phases,
because a breakdown that sums to exactly 100% by absorbing its own residue is a
tidier lie than a profile with a hole in it.

**Nothing here explains *why* verification and window maintenance are expensive.**
The measurement locates the cost; it does not attribute it to a mechanism. The
baseline document's suggestion that the bucket substrate's memory behaviour is
involved remains **a hypothesis and is not tested by this work** — no cache
miss, no branch misprediction and no memory stall was measured, and none can be
on this host. Confirming or refuting it requires native Linux `perf` or Intel
VTune on Windows, as `profiling.md` records.

What *is* established by measurement: the dominant overhead is the candidate
window and its verification, not comparisons, not allocation, and not bucket
traversal.

## Reproducing

```bash
cmake --build build/release --target lpq_profile lpq_profile_noinstr

./build/release/bin/lpq_profile --dataset datasets/processed/dimacs_ny.lpqgraph \
    --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
    --pq lpq --repetitions 3

./build/release/bin/lpq_profile_noinstr --dataset datasets/processed/dimacs_ny.lpqgraph \
    --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
    --pq lpq --repetitions 3
```

Swap `--pq binary_heap` for the baseline. The ratio of the two wall clocks is
the instrumentation overhead; the probe-pair cost is calibrated and printed by
every run, so it never has to be taken on trust from this document.
