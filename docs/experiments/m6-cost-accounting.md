# M6: where the time goes, and what each timed number contains

Measured 2026-10-13 on commit `450804238ca8786fb45bdce9f70991fc1765d55f` (branch `completion/m5-candidate`), from a
clean worktree and fresh builds. Methods first, then results, then every pre-registered hypothesis with its outcome,
including the ones that failed or were not settled. **This document makes no claim that the learned queue, or online
learning, is faster than anything.** On the two graphs measured the learned queue was slower than the radix heap, and
that is the only direction these runs support.

Everything below is a measurement on **one shared virtual machine** (Intel Xeon @ 2.8 GHz, 4 vCPUs visible, `nproc` 1,
no frequency scaling interface, no PMU). A 60-sample busy-loop probe taken before the runs varied 2.2x from fastest to
slowest sample, and steal ticks were non-zero. **Absolute times are therefore host-specific and are not comparable
across invocations**; only the paired ratios measured inside one process are used for comparisons, and their spread
across independent invocations is reported. Hardware counters (cycles, instructions, cache and branch misses, memory
bandwidth) are **not measured**; no figure here is derived from them.

## 1. What each number is

| quantity | what it contains | where it is recorded | in the timed samples? |
|---|---|---|---|
| **T_search** | one search call: for the LPQ every push and pop, bucket selection, candidate window, verification, correction and fallback, plus the graph traversal | `timing.median_ns` and the raw `timing.samples_ns` of each record (median of 7 repetitions, 3 warm-ups discarded and kept) | yes, by definition |
| per-push scoring of `lpq_online` | the on-demand score of each pushed node | nowhere separately | **yes** (see H8) |
| scorer build for `lpq_model` | one model inference per node, once per graph | not recorded by `lpq_benchmark`; estimated below | no (built before the timed region) |
| `lpq_optimized` weight profile and calibration | `profile_ns`, `calibration_ns` | `queue_config` of every `lpq_optimized` record | no |
| online update | learner `flush` after the query's last repetition | `ml.online_update_ns` of `lpq_online` records | no |
| reference validation, model and graph load | | not recorded | no |
| phase shares | ticks per phase, from `lpq_profile` only | its text output | not from the benchmark |

`ml.feature_extraction_ns`, `ml.inference_ns` and `ml.verification_ns` are **always 0** in `lpq_benchmark` records
(D-26): the benchmark does not attribute per-component ML time, so the only component-level information is the phase
profile (a separate binary) and `online_update_ns`.

**Amortised estimates.** Where a one-time cost is spread over Q queries below, the figure is `T_search + one-time cost / Q`
and is **an amortised estimate, not a directly measured end-to-end time**. It assumes the one-time cost is paid once per
graph and machine and that Q queries follow.

## 2. Protocol (pre-registered, approved before any timing)

* **Fresh clean builds, provenance gated.** New worktree detached at the candidate SHA; `cmake --preset release` and
  `--preset release-noinstr`, each printing `git commit: 450804238ca8…` and `git state: clean`; no copied build directories.
  Before and after the runs `HEAD` and `git status --porcelain` were checked (clean, unchanged). Every record of the 7,200
  carries `git_commit` 450804238c… and `git_dirty` false, and `cpu_pinned` true. Hashes of the four binaries, the graphs,
  manifests and models, and of every raw result file, are in `m6/provenance.txt`. The stale or dirty binaries from earlier
  milestones were not used.
* **Pinning and pairing.** `--cpu-core 2 --require-pinning`; `--interleave` (query-major, seeded shuffle of the row order per
  query); seven measured repetitions, three warm-ups; reference validation on for every query; all rows of a block in one
  process. A* was excluded by instruction.
* **Blocks.** A: `dimacs_ny`, 100 queries, 9 rows, 3 invocations (seeds 20260923 to 20260925). B: the same rows on
  `snap_facebook`. C: counter overhead, `release` against `release-noinstr`, alternated REL, NI, REL, NI. D: `lpq_profile`
  against `lpq_profile_noinstr`, two rounds, for `lpq` and `binary_heap`. E: `lpq_online` against `lpq_model` on
  `snap_facebook`, 100 queries, one invocation with update mode `disabled` and one with `periodic`. Scorer-build probe on `dimacs_ny`.
* **Rows:** `radix_heap` (ratio anchor), `binary_heap`, `four_ary_heap`, `bucket_classical` (the classical bucket queue,
  derived width), `lpq` (constant scorer), `lpq_model` (protocol-C static scorer), `lpq_direct_nonml` (in-place extraction,
  front 0, exact only), `lpq_optimized` (the calibrated v2 option), `replay_floor` (diagnostic: the search with the queue
  work removed, `v2-results.md` section 1).
* **Statistics.** Per query the harness gives the median of 7 repetitions. Across queries: median and MAD. A paired ratio
  per query (row A over row B), its median, a 95% bootstrap CI over queries (10,000 resamples, explicit seeds), the
  fraction of queries on which A is faster, and a Holm-adjusted permutation p-value per invocation. **Reportability rule:**
  a difference is "established" only if the sign agrees in every invocation of the block and the pooled cluster-bootstrap
  CI excludes 1; otherwise "not distinguishable". No record and no invocation was dropped. 2,327 of 7,200 records (32.3%)
  were flagged noisy by the harness and are included.
* **Anomaly rule for the fourth invocation** (`m6/anomaly-rule.txt`, written before Block A): an invocation is anomalous if
  its anchor differs by more than 25% from the median anchor or more than half its records are noisy. **None was**
  (largest anchor deviation 8.3%; noisy fractions 31% to 34%), so no fourth invocation was run.
* **Deviations from the approved plan, stated.** Tests and fuzzers were switched off at configure time to shorten the build
  (compiler flags unchanged). The `.lpqprofile` cache was deleted before the first run so the profile cost was recorded; the
  Facebook profile was first computed by the smoke test, so its Facebook rows show `profile_source: cached` with the cost
  recorded at first computation. Hypothesis H6 was given a threshold (at least 50% of pop ticks) after I had seen a
  three-query smoke profile on Facebook and before the NY profiles were analysed. The scorer-build probe uses `--no-validate`,
  a flag documented as not for reported results, because it times a process and not a search.

## 3. Results: paired comparisons

Ratios are row A over `radix_heap` unless stated; above 1 means A is slower. Full per-invocation tables, with CIs, the
fraction of queries won and adjusted p-values, are in `m6/analysis-output.md`. All 7,200 records were exact against the
reference.

| A / B | `dimacs_ny` pooled median [95% CI] | `snap_facebook` pooled median [95% CI] | verdict (both graphs) |
|---|---|---|---|
| `binary_heap` / `radix_heap` | 1.302 [1.278, 1.346] | 1.390 [1.358, 1.438] | established slower |
| `four_ary_heap` / `radix_heap` | 1.265 [1.231, 1.290] | 1.303 [1.274, 1.333] | established slower |
| `bucket_classical` / `radix_heap` | 1.211 [1.183, 1.238] | 3.303 [3.025, 3.635] | established slower |
| `lpq` / `radix_heap` | 3.203 [3.066, 3.290] | 8.058 [7.495, 8.818] | established slower |
| `lpq_model` / `radix_heap` | 3.199 [3.066, 3.302] | 8.723 [8.102, 9.338] | established slower |
| `lpq_direct_nonml` / `radix_heap` | 1.400 [1.353, 1.434] | 2.196 [2.101, 2.288] | established slower |
| `lpq_optimized` / `radix_heap` | 0.998 [0.979, 1.010] | 1.001 [0.993, 1.005] | not distinguishable |
| `replay_floor` / `radix_heap` | 0.537 [0.520, 0.553] | 0.786 [0.776, 0.799] | established faster (diagnostic) |
| `lpq_model` / `lpq` | 0.984 [0.967, 1.005] | 1.095 [1.079, 1.112] | NY not distinguishable; **FB established slower** |
| `lpq_direct_nonml` / `lpq` | 0.439 [0.427, 0.465] | 0.274 [0.264, 0.284] | established faster |

Absolute medians of the per-query medians, `dimacs_ny`, invocation 1 (host-specific): `radix_heap` 18.7 ms, `binary_heap`
26.9, `four_ary_heap` 24.0, `bucket_classical` 24.7, `lpq` 64.4, `lpq_model` 62.6, `lpq_direct_nonml` 30.0,
`lpq_optimized` 19.9, `replay_floor` 10.6; the `radix_heap` anchor was 18.7, 18.9 and 20.5 ms in the three invocations.
Comparison counts are exact and reproduce the historical ones: `radix_heap` 71,839,857; `binary_heap` 203,801,439; `lpq`
177,580,603; `lpq_model` 177,732,303 (`dimacs_ny`).

* **The radix heap is the fastest classical row on both graphs**, and `lpq_optimized` is the radix heap. On both graphs
  its autotuner found no pooled geometry that beat radix by the required margin (`resolved_queue: radix_heap`, all three
  invocations); the first invocation calibrated (`autotune`), the others read the cache (`autotune_cached`).
* **Noise floor of the pairing method.** `lpq_optimized` runs the same queue as `radix_heap`, so their ratio is a null
  comparison: 0.998 [0.979, 1.010] on `dimacs_ny` and 1.001 [0.993, 1.005] on `snap_facebook`. Differences of a couple of
  per cent between rows are therefore at the resolution of the method.
* **The learned scorer did not make the queue faster.** `lpq_model` against `lpq` (same queue, constant scorer replaced by
  the trained model) is not distinguishable on `dimacs_ny` and is about 9.5% slower on `snap_facebook`, where it also does
  0.8% more comparisons (83,240,154 against 82,595,639). Against the radix heap both are 3x and 8x slower.
* The in-place variant `lpq_direct_nonml` is much faster than `lpq` (0.44x and 0.27x) and still slower than the radix heap
  (1.40x and 2.20x): the existing simpler extraction policy removes most of the LPQ-specific overhead and not all of it.
* `replay_floor` is the diagnostic that the queue work is roughly half of a radix search on `dimacs_ny` (0.54x) and about a
  fifth on `snap_facebook` (0.79x). The `v2-results.md` figures were 0.47 to 0.51x and 0.72 to 0.79x on another host and
  build; the direction agrees and the values are not identical.

## 4. Counter overhead, measured before the phase profile was read (Block C)

`release` (LPQ and pq-operation counters compiled in, the configuration all published timings use) against
`release-noinstr`, binaries alternated, same seed within a pair, rows `radix_heap`, `binary_heap`, `lpq` on `dimacs_ny`.
Counters-on over counters-off, pooled median [95% CI]: `radix_heap` 1.085 [1.050, 1.131], `binary_heap` 1.057
[1.033, 1.077], `lpq` 1.073 [1.046, 1.120]. Counters-on was slower on 61% to 65% of queries. In the second pair the
`binary_heap` and `lpq` intervals include 1. The counters thus cost roughly 6% to 9%, differently per row, which shifts a row ratio by
about 1% to 3% (derived, not measured: `lpq`/`radix_heap` would be about 3.24 instead of 3.20 without counters). The
`release-noinstr` build records 0 comparisons, as designed.

## 5. Phase profile (Block D), read after Block C

`lpq_profile` (phase probes) against `lpq_profile_noinstr` (same source, probes compiled out), three passes over the 100
`dimacs_ny` queries, constant-scorer LPQ and the binary heap, two rounds, on this host. **The older profile was taken on a
different machine and is not reused.**

| queue | round | wall without probes | wall with probes | inflation | probe pair (ticks) |
|---|---|---|---|---|---|
| `lpq` | 1 | 30.83 s | 36.26 s | 1.18x | 52.7 |
| `lpq` | 2 | 27.26 s | 34.40 s | 1.26x | 63.7 |
| `binary_heap` | 1 | 10.19 s | 12.93 s | 1.27x | 45.8 |
| `binary_heap` | 2 | 10.49 s | 12.66 s | 1.21x | 46.3 |

So the probes added roughly 18% to 27% to the wall clock, which is inside every share below (the tool never subtracts it).
Shares are of ticks, not durations. For the LPQ, **pop is about 86.5% of push+pop ticks, and within pop** (rounds 1 and 2):
window maintenance 36.9% and 37.5%, verification scan 37.2% and 37.2%, fallback scan 0.02% and 0.01%, bucket traversal 6.3%
and 5.8%, remove from bucket 6.9% and 6.8%, unattributed 12.7% and 12.7%. Window maintenance plus verification plus fallback is
74.0% and 74.7% of pop ticks. Several phase boundaries are at or near the probe floor, so the small phases are the least reliable.
Not profiled: `lpq_model`, `lpq_direct`, `lpq_online`, and any graph other than `dimacs_ny`.

## 6. Online learning (Block E) and the update cost

`snap_facebook`, 100 queries, protocol-C model, in one process per mode with `radix_heap` and `lpq_model`. All 600 records exact.

| mode | search time `lpq_online` / `lpq_model` | online faster on | update cost per query |
|---|---|---|---|
| `disabled` (frozen) | 1.288 [1.258, 1.307] | 10% of queries | 0 (median, max 0.1 us); weights unchanged on all 100 |
| `periodic` | 1.309 [1.288, 1.335] | 5% of queries | median 0.80 ms, MAD 0.18 ms, max 1.71 ms; weights changed on all 100; 577 updates by the last query |

* The update runs after the last timed repetition of each query and is recorded once per query in `ml.online_update_ns`;
  every record has exactly 7 timed samples, so it is neither inside them nor multiplied by the repetition count. **Labelled
  estimate, not a measured end-to-end time:** search plus update per query is 5.00 ms + 0.00 ms for the frozen row and
  5.00 ms + 0.80 ms = 5.81 ms for the periodic row (median T_search plus median update), against 3.70 to 3.86 ms for
  `lpq_model` in the same processes.
* **The frozen `lpq_online` is not as fast as `lpq_model`; it is about 29% slower in search time.** Its results and comparison
  counts are identical (milestone M5); its clock is not. The consistent explanation is the design: `OnlineNodeScorer`
  computes each node's score on demand, inside the search, while `StaticNodeScorer` reads a precomputed float. I did not
  profile `lpq_online`, so the share of that explanation is **not measured**. The periodic row's extra difference from the
  frozen row (1.309 against 1.288) is not separated from noise.
* This says nothing about whether updating helps: no row here compares an updated model's search against a frozen one on
  anything but the same queries in the same order, and **online-learning performance benefit is not established**.

## 7. One-time costs and the amortised estimates

| cost | value | how obtained | uncertainty |
|---|---|---|---|
| `lpq_model` scorer build, `dimacs_ny` (264,346 nodes), including model load | **about 35 ms** | median, over 20 triples, of the process wall time of a one-query `lpq_model` run minus the mean of the `lpq` runs either side; a null comparison (`lpq` against `lpq`) was run alongside | 95% bootstrap CI of the median 24 to 44 ms; positive in 17 of 20 triples; **the null pairs differ by MAD 20 ms and range -62 to +68 ms**, so this is a coarse, weakly resolved estimate; it also contains the (small) search difference between the two queues and was not measured for `snap_facebook` |
| `lpq_optimized` weight profile, `dimacs_ny` | 13.9 ms | recorded by the engine | single computation |
| `lpq_optimized` calibration, `dimacs_ny` | 3,950.9 ms, once; 0 afterwards (cached beside the graph) | recorded by the engine | single measurement; the outcome was "use the radix heap" |
| `lpq_optimized` profile and calibration, `snap_facebook` | 4.6 ms and 123.2 ms | recorded | single measurements |

Amortised per-query estimates (`one-time cost / Q`, added to the measured T_search; estimates, not measurements):

| row | Q = 100 | Q = 1,000 | Q = 10,000 | T_search it is added to |
|---|---|---|---|---|
| `lpq_model`, `dimacs_ny` (35 ms) | 0.35 ms | 0.035 ms | 0.0035 ms | about 63 to 70 ms |
| `lpq_optimized`, `dimacs_ny` (3,964.8 ms) | 39.6 ms | 3.96 ms | 0.40 ms | about 19.5 to 20.5 ms |
| `lpq_optimized`, `snap_facebook` (127.8 ms) | 1.28 ms | 0.128 ms | 0.0128 ms | about 0.5 ms |

On both graphs the calibration bought nothing, because the decision was the radix heap: its cost is pure overhead there and
only becomes small relative to a search after thousands of queries.

## 8. Pre-registered hypotheses and their outcomes

| | prediction | outcome |
|---|---|---|
| H1 | `radix_heap` faster than `binary_heap` on NY | **Held.** 1.302 [1.278, 1.346], established (and 1.390 on Facebook). |
| H2 | `lpq` clearly slower than `radix_heap` on NY | **Held.** 3.20x, established (8.06x on Facebook). |
| H3 | `lpq_model` and `lpq` not distinguishable | **Held on NY, failed on Facebook.** NY 0.984 [0.967, 1.005], signs disagree across invocations; Facebook 1.095 [1.079, 1.112], established slower. The hypothesis was not tied to a graph; its stated basis was the NY comparison counts. |
| H4 | `lpq_direct:front=0` faster than `lpq` | **Held.** 0.439 on NY and 0.274 on Facebook, established. It remains slower than `radix_heap`. |
| H5 | `lpq_optimized` resolves to `radix_heap` on NY | **Held.** `resolved_queue: radix_heap` in all three invocations (also on Facebook); ratio to radix 0.998, not distinguishable. |
| H6 | window and verification dominate LPQ pop time | **Held**, with the threshold fixed as described in section 2: 74.0% and 74.7% of pop ticks, constant-scorer LPQ, `dimacs_ny` only, probes inside the shares. |
| H7 | counter overhead under 10% | **Not established.** Point estimates 5.7% to 8.5%, but the 95% upper bounds for `radix_heap` (13.1%) and `lpq` (12.0%) exceed 10%. |
| H8 | frozen `lpq_online` has the same search time as `lpq_model` | **Failed.** 1.288 [1.258, 1.307]; the periodic run agrees in sign and size. Two invocations (by design), not three. |

## 9. What is established, and what is not

Established by the pre-registered rule, on these two graphs and this host: the radix heap is the fastest classical queue and
`lpq_optimized` resolves to it; the constant-scorer LPQ is 3.2x (`dimacs_ny`) and 8.1x (`snap_facebook`) slower than the radix
heap; the trained static scorer does not make it faster (indistinguishable on NY, slower on Facebook); the in-place
extraction variant removes most but not all of the gap; about three quarters of LPQ pop time is window maintenance and
verification; a frozen online scorer costs about 29% more search time than the precomputed one.

Not established: any benefit of learning, online or static; the counter overhead being under 10%; anything about A*, other graphs,
other weight distributions, other hosts, or hardware-counter behaviour.

## 10. Acceptance gaps

* **Component-level ML time is not recorded by the benchmark** (`ml.feature_extraction_ns`, `inference_ns`, `verification_ns`
  are 0; D-26). The handoff asks for "feature extraction, model inference, candidate/window maintenance, verification,
  correction, fallback" separately; only the last four are covered, by the phase profile, and only for the constant-scorer
  LPQ on `dimacs_ny`. Feature extraction and inference cost for the static scorer is the scorer-build estimate in section 7;
  its in-search part for `lpq_online` is not isolated.
* **No change was made to the result schema or engine** (no `scorer_build_ns`), as instructed; the scorer-build figure is
  therefore a coarse process-level estimate for one graph.
* The online block has one invocation per mode, on one graph, 100 queries; the three-invocation rule was not applied to it.
* Single host, shared VM, 32% of records noise-flagged; the radix anchor drifted 8% between invocations. Hardware counters
  not measured.
* The test-only accounting checks exist (`python/tests/test_cost_accounting.py`, 4 tests): they pin that the online update
  cost is per query and outside the timed samples, that a static row records none, that `lpq_optimized` reports its one-time
  costs beside the samples, and that the per-component ML fields are 0 today. They are characterisation tests of the harness,
  not a proof that the accounting is complete.
* Reproduction: `m6/run_all.sh` is the exact master script, `m6/analysis.py` regenerates `m6/analysis-output.md` from the raw files;
  the raw files themselves are not tracked (`results/` is ignored) and are identified by hash in `m6/provenance.txt`.
