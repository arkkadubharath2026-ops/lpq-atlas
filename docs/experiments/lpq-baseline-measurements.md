# First measurements of the learned priority queue against the classical baselines

This records the first end-to-end measurements of the LPQ against the baseline
queue matrix (PRD.md §19) on real datasets. It is a **negative result for the
central hypothesis as currently implemented**, and it is recorded here in full
rather than deferred, because the finding shapes what the remaining milestones
should try to establish.

Everything below was measured. Nothing is estimated, modelled or extrapolated.

## What was measured

| | |
|---|---|
| Datasets | `snap_facebook` (4,039 nodes / 88,234 edges), `dimacs_ny` (264,346 nodes / 733,846 arcs) |
| Algorithm | Dijkstra, exact mode, single-target |
| Queues | binary, 4-ary, pairing, Fibonacci, radix heaps, and the LPQ |
| LPQ configuration | `ConstantNodeScorer` — **no learned model**; see the caveat below |
| Correctness | every query validated against the reference implementation; all passed |
| Commit | the working tree at `dffb95c` (the runner warns that the tree was dirty) |
| Host | Intel i5-1235U laptop, single pinned core, Windows/GCC 16.1 Release |

Two kinds of number appear here and they have very different standing.

**Comparison counts are exact.** They are deterministic properties of the
algorithm and the query set, independent of machine load, and reproduce
byte-for-byte across runs.

**Timings are measurements** and carry noise. The figures below come from a run
with 100 queries, 11 measured repetitions, 3 discarded warm-up repetitions and
`--require-pinning`. The runner still flagged 205 of 600 query measurements as
exceeding its within-query noise threshold, which is what a consumer laptop
does; the within-query relative MAD was 1.2%–6.9% depending on the queue. The
effects reported below are one to two orders of magnitude larger than that, so
the conclusions survive the noise, but no figure here should be quoted to more
than about two significant figures.

## Comparison counts

Exact, over 1,000 manifest queries per dataset.

| queue | `snap_facebook` | `dimacs_ny` |
|---|---:|---:|
| radix heap | 13,823,163 | 770,196,186 |
| pairing heap | 49,784,628 | 1,798,398,838 |
| **LPQ** | **798,334,612** | **1,948,570,730** |
| binary heap | 52,570,520 | 2,191,917,885 |
| 4-ary heap | 52,820,571 | 2,380,890,373 |
| Fibonacci heap | 59,703,365 | 2,635,910,119 |

Against the binary heap the LPQ does **15.2× more** comparisons on the social
graph and **11.1% fewer** on the road network. Its position reverses completely
between the two graph types.

The mechanism is visible in comparisons per extraction:

| queue | `snap_facebook` | `dimacs_ny` |
|---|---:|---:|
| radix heap | 3.9 | 5.2 |
| binary heap | 25.9 | 16.9 |
| LPQ | 225.9 | 13.2 |

This is the substrate argument in `bucket_substrate.hpp`, confirmed by
measurement rather than asserted. On a road network (average degree ≈ 3) the
candidate window does what it was designed to do: 13.2 entries examined per
extraction against the binary heap's 16.9. On a social graph (average degree
≈ 44) buckets fill deeply, the verification scan dominates, and the window
almost never wins.

**Dense graphs are therefore a genuine limitation of this design, not a tuning
problem.** No window size fixes a substrate whose buckets hold hundreds of
entries; the scan is the cost.

## Wall-clock

`dimacs_ny`, 100 queries (verified to be the exact prefix of the 1,000-query
manifest, so the two runs are comparable), 11 repetitions, 3 warm-up, pinned.

| queue | median ns | across-query MAD | comparisons |
|---|---:|---:|---:|
| binary heap | 7,805,800 | 5,350,000 | 203,801,439 |
| 4-ary heap | 8,814,950 | 6,246,900 | 221,402,601 |
| radix heap | 10,217,950 | 6,930,800 | 71,839,857 |
| pairing heap | 16,367,200 | 11,086,900 | 167,312,239 |
| Fibonacci heap | 30,576,600 | 23,364,900 | 245,021,912 |
| LPQ | 36,308,050 | 29,899,800 | 177,580,603 |

The across-query MAD is large for every queue because per-query difficulty on a
road network genuinely varies by orders of magnitude. That is workload
heterogeneity, not measurement noise, which is precisely why the comparison
below is paired.

## Paired comparison against the binary heap

Paired permutation test on the per-query medians, 10,000 resamples, seed
20240923, Holm–Bonferroni corrected across the five comparisons. `median
difference` is `binary_heap − B`, so a negative value means B is slower.

| B | speedup of B over binary | median difference (ns) | 95% CI (ns) | queries where B faster | adjusted p |
|---|---:|---:|---:|---:|---:|
| 4-ary heap | 0.868 | −1,106,400 | [−1,271,050, −925,400] | 3% | 5.0e−04 |
| radix heap | 0.751 | −2,286,050 | [−2,802,400, −1,817,600] | 6% | 5.0e−04 |
| pairing heap | 0.514 | −7,937,000 | [−9,385,200, −5,486,700] | 1% | 5.0e−04 |
| Fibonacci heap | 0.251 | −22,756,100 | [−30,337,000, −20,308,800] | 0% | 5.0e−04 |
| **LPQ** | **0.219** | **−28,343,400** | **[−35,324,700, −21,307,700]** | **1%** | **5.0e−04** |

Every confidence interval excludes zero by a wide margin. The reported p-values
sit at the resolution floor of a 10,000-resample permutation test after
correction, so they should be read as "smaller than this test can measure", not
as a precise value.

The LPQ is **≈4.6× slower than the binary heap** on this workload, and is slower
on 99% of individual queries.

## The finding

**Comparison count does not predict wall-clock time on road networks.**

The LPQ performs 12.9% *fewer* comparisons than the binary heap on this query
set and is 4.6× slower. The same divergence appears in the classical baselines
and is therefore not peculiar to the LPQ: the radix heap performs 2.8× fewer
comparisons than the binary heap and is still 1.33× slower.

This has a direct and uncomfortable consequence for the project's premise. The
argument for a learned priority queue is that prediction lets the queue examine
fewer entries per extraction. On the road networks the LPQ **already** examines
fewer entries than the baseline it loses to, with no model at all. A perfect
predictor could only reduce that number further — it cannot recover a 4.6×
deficit, because the deficit is not made of comparisons. It is per-extraction
overhead: candidate-window bookkeeping, the verification path, and the memory
behaviour of the bucket substrate and its overflow ring.

Improving the model is therefore not the highest-value next step. Establishing
*where the time actually goes* is.

## What this does not show

These limits are stated because the result above would otherwise be read more
broadly than it deserves.

- **No model was involved.** This is the LPQ *structure* with a constant scorer,
  which was deliberate: it isolates the machinery from prediction quality. A
  learned scorer changes the comparison counts, not the per-extraction overhead
  that appears to dominate.
- **Exact mode only.** Approximate mode trades correctness for fallback-scan
  rate and has not been measured end to end on these datasets.
- **One host, one core, one compiler.** An Intel i5-1235U laptop under Windows.
  No hardware counters were collected because WSL2 exposes no CPU PMU; see
  `profiling.md`. Cycles, cache misses and branch misses are *not measured*, and
  the claim about memory behaviour above is therefore a hypothesis consistent
  with the timings, not a measured attribution.
- **Two datasets.** One social graph, one road network. The `dimacs_col`,
  `dimacs_fla`, `snap_roadnet_pa` and `snap_roadnet_ca` graphs are prepared but
  were not measured here.
- **Dijkstra only.** A\* was not run; the LPQ's interaction with a heuristic is
  untested.

## Reproducing

```bash
python scripts/data/prepare_datasets.py dimacs_ny

lpq_benchmark --dataset datasets/processed/dimacs_ny.lpqgraph \
              --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
              --pq all --repetitions 11 --warmup 3 --require-pinning \
              --output results/raw/ny_timing_clean.json
```

Raw per-query records are written to `results/raw/` and are git-ignored for
size; every sample is retained and none are discarded, including the ones the
runner flags as noisy.
