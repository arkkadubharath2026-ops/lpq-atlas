# v2: what was measured, and what it supports

Everything below was measured in this repository's v2 work on one cloud VM
(Intel Xeon @ 2.8 GHz, 4 vCPU, KVM, GCC 13.3, Release `-O3`, no `-march=native`).
Raw results are in `results/raw/{v2,policy,headline}/` (git-ignored by the
repo's own rule; each file records the commit, hardware and every sample). **No
hardware counters exist on this host**, so none are reported. Cloud timing is
development-stage evidence: it drifts by tens of percent between runs, which is
why every comparison below is *paired and interleaved* (all configurations of a
query run back to back, in a seeded random order, on one pinned core).

## Short answer

* **The final system, `lpq_optimized`, is faster than the strongest classical
  baseline (radix heap) on one workload we can identify and verify:** Dijkstra on
  DIMACS New York. Paired over 100 queries it takes **0.916x the radix heap's
  time** (95% CI 0.892-0.939, i.e. 6.1%-10.8% less time), is faster on 79% of
  queries, and is **1.50x faster than the binary heap** and **3.4x faster than the
  original LPQ**. All answers matched the reference Dijkstra.
* **It does not beat radix elsewhere.** On Facebook, DIMACS COL and under A\* it
  resolves to the radix heap, so it equals radix by construction (those rows
  measure noise: 0.977-1.050).
* **The gain comes from queue engineering plus a per-graph measured choice, not
  from a learned predictor.** A trained model contributes nothing measurable (below).
* The original design (candidate window + verification + trained scorer) is
  **3.1x slower than radix** on New York and stays slower after every
  optimisation we tried.

## 1. Where the time goes (replay floor)

A diagnostic queue that replays a recorded pop sequence and ignores pushes runs
the same search with queue work removed. On the DIMACS road graphs it takes
**0.47-0.51x the radix heap's time** (0.56 on PA, 0.67 on RoadNet-CA): about half
of a radix search is the queue, so no queue (learned or not) can save more than
roughly half there. Facebook: 0.72-0.79x.

## 2. Reproduced baseline (before changing anything)

Comparison counts match the historical record to the unit (radix 71,839,857;
binary heap 203,801,439; original LPQ 177,580,603) and the static scorer's
held-out ROC-AUC reproduces at 0.5337 on New York. Wall-clock: original LPQ is
2.75-3.2x slower than radix on this machine.

## 3. Audit findings that changed the plan

* `OnlineLearner::maybe_update` / `flush` have **no call site outside tests**.
  `--adaptive` attaches a learner that only *records* events; it holds no model,
  and the adaptive configuration uses the constant scorer. No model weight changes
  in any benchmark or API run. Online learning is therefore not claimed anywhere.
* The earlier "adaptive controller cuts comparisons 34%" equals the plain bucket
  queue: 116,552,995 comparisons for the classical bucket queue vs 116,608,480
  adaptive. It is the retreat to a classical scan, not learning.
* On New York only 7,119 of 13.8M pops reached the classical fallback; ~63% of
  queue time was candidate-window maintenance plus verification (profiler, New York).

## 4. Contribution isolation (New York, 30 queries, paired vs radix)

| configuration | time vs radix (95% CI) |
|---|---|
| original LPQ, constant scorer | 3.10x [2.74, 3.45] |
| original LPQ, trained scorer | 3.17x [2.71, 3.61] |
| in-place "direct" extraction, constant scorer | 1.46x [1.36, 1.57] |
| in-place "direct", trained scorer (front 32) | 1.66x [1.56, 1.75] |
| in-place "direct", trained scorer (front 0) | 1.45x [1.35, 1.56] |
| classical bucket queue (legacy width) | 1.17x [1.11, 1.24] |
| pooled bucket queue, width 1 | 0.95x [0.88, 1.03] |

* Queue engineering (in-place verification instead of copy-sort-verify) roughly
  halves the original LPQ's cost (3.10x -> 1.46x). It does not reach radix.
* The trained scorer gives **no gain** (3.10 -> 3.17; 1.46 -> 1.66 because
  maintaining a score-ordered front costs more than the hint saves).
* A sequential (non-interleaved) width sweep earlier in this work concluded that
  narrow buckets were clearly better; interleaving showed that was host drift.
  The sequential result is not used anywhere.

## 5. The pooled bucket queue across graphs (Dijkstra, paired vs radix)

Best of width 1/2/4/8/16, geometric-mean time ratio (CI), 30 queries each:
New York 0.90 [0.85, 0.96]; NY topology with uniform weights <= 4096: 0.87
[0.83, 0.92]; DIMACS COL 0.93 [0.87, 1.00]; PA 0.97 [0.93, 1.02];
Facebook 1.05 [0.996, 1.13]; DIMACS FLA 0.99-1.06 (no gain); RoadNet-CA 1.19
(worse). Wider buckets are catastrophic on small-weight graphs (71-133x slower on RoadNet-CA).
Under **A\*** the pooled queue did not beat radix (1.00-1.08 on NY/COL/FLA).
Several of the 12 `derived_*` graphs (same topology, synthetic weights; clearly
labelled, Dijkstra only) were measured to map where the effect holds.
Part of this campaign ran two pinned processes concurrently (cores 2 and 3);
ratios are paired within a process but absolute times are not comparable.

## 6. Learned configuration policy: a negative result

A ridge policy over graph weight statistics (7 features), trained on measured
runtimes of 17 graphs from 6 topologies, was evaluated **nested
leave-one-topology-out** (a held-out graph's topology and all derived variants of
it are excluded from training; margin and ridge strength chosen by an inner loop).
It **never beat radix** (0 of 17 held-out graphs; geometric-mean realised ratio
1.010; it lost on three Facebook-type graphs). Even the per-graph oracle would
gain only 3.2% on average. With so few topologies, weight statistics cannot
predict where bucketing wins, so the policy is **not used by default**. It remains
available (`--policy`) and its report is `models/policy/queue_policy_v1.report.json`.

## 7. What `lpq_optimized` does

Once per graph and machine it runs a short calibration on 12 sampled queries:
every candidate (radix, pooled width 1/2/4) is timed back to back in shuffled
order, and a pooled queue is chosen only if it beats radix by >= 3% (geometric
mean) and wins on >= 60% of pairs; otherwise it keeps radix. The decision is
cached beside the graph keyed by graph checksum, CPU and settings, and every
result states the resolved queue, why, and the calibration cost (2.7 s on New
York, once; 0 afterwards). It is exact: every candidate is a correct queue, so a
poor choice is slow, never wrong. A\* resolves to radix.

## 8. Headline (alone on one pinned core, 7 repetitions, all exact)

| workload | queries | radix median | `lpq_optimized` vs radix (95% CI) | resolved to |
|---|---|---|---|---|
| NY, Dijkstra | 100 | 12.66 ms | **0.916** [0.892, 0.939], faster on 79% | pooled, width 2 |
| NY, A\* | 100 | 6.15 ms | 0.977 [0.946, 1.009] | radix (same queue) |
| Facebook, Dijkstra | 100 | 0.35 ms | 1.008 [0.983, 1.034] | radix (same queue) |
| COL, Dijkstra | 30 | 31.77 ms | 1.050 [0.980, 1.125] | radix (same queue) |

On New York Dijkstra, relative to radix: binary heap 1.379x, 4-ary heap 1.390x,
original LPQ 3.087x, direct non-ML LPQ 1.388x, replay floor 0.471x. The three
rows that resolved to radix are a null comparison; their spread (0.977-1.050) is
the noise floor of this host and a result must clear it.

## 9. Not shown / limits

* One graph with a verified gain (plus its same-topology synthetic variant); COL
  is borderline. Not shown on other road networks, on A\*, or on other hardware.
* 100 queries on New York; 30 elsewhere. Cloud VM, no PMU, no dedicated machine.
* The autotuner's calibration (12 queries) can mis-pick on noisy hosts; the
  fallback is radix. Re-run `scripts/bench/run_headline.sh` locally: the cached
  decision is per machine, so a local run re-calibrates.
* Online learning is not part of the optimized path and was not shown to help.
* Windows setup scripts are unverified on Windows.
