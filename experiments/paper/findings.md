# A learned priority queue for shortest paths: what was measured

**Summary of the result: the central hypothesis is not supported by this
project's own measurements.** The learned component works — it predicts its
target, and it is measurably faster than the control — but it is attached to
machinery that costs far more than prediction can recover, sitting on a
classical substrate that had already won most of the available advantage before
any learning was involved.

Every figure below was measured on this system. Nothing is estimated,
extrapolated or modelled. Where something could not be measured, it says so.

---

## 1. What was built

A bucket-based priority queue with a learned scorer, a candidate window, and a
verification engine that guarantees the extracted element is the true minimum
(exact mode) or within a stated slack of it (approximate mode). The classical
queue is the correctness anchor throughout; the learned component is only ever a
hint, and verification is what makes it safe to be wrong.

The measurement infrastructure is the larger part of the work: a pinned
single-core benchmark harness reporting median and MAD, a phase profiler that
reports its own overhead, exact operation counters, paired permutation testing
with Holm correction, and a four-command reproduction pipeline.

## 2. The central finding

Read as a ladder rather than as a pairwise comparison, on `dimacs_ny`
(264,346 nodes / 733,846 arcs, 100 queries, 11 repetitions, one pinned process):

| rung | what it adds | comparisons | vs previous |
|---|---|---:|---:|
| `binary_heap` | classical comparison-ordered heap | 203,801,439 | 1.000× |
| `radix_heap` | + bucketing by key | 71,839,857 | **0.352×** |
| `lpq` | + candidate window and verification | 177,580,603 | **2.472×** |
| `lpq_model` | + a trained model | 177,732,303 | 1.001× |

**Bucketing alone wins 2.8× on comparisons. The candidate window and
verification engine give 2.5× of it straight back.**

The pairwise framing hides this. Against a binary heap the learned queue looks
mildly favourable — 12.9% fewer comparisons — and that number is a large win
from the substrate minus a large loss from the machinery on top of it. The
learned queue is not narrowly beating a classical heap; it is squandering most
of an advantage its own substrate had already won.

Wall-clock, paired permutation test, Holm-corrected:

| step | median difference | 95% CI | B faster on |
|---|---:|---|---:|
| `binary_heap` → `radix_heap` | −1,362,800 ns | [−2,070,900, −893,800] | 8% |
| `radix_heap` → `lpq` | **−24,533,700 ns** | [−35,204,400, −18,621,200] | **0%** |
| `lpq` → `lpq_model` | +1,427,450 ns | [+832,200, +2,128,800] | 80% |

The window and verification engine cost ≈24.5 ms per query and are slower on
**every query measured**. The trained model recovers ≈1.4 ms of that — about 6%
of what the machinery it steers spends.

## 3. Why: where the time actually goes

Measured directly with a phase profiler (proportions of queue time, `dimacs_ny`):

| phase | share |
|---|---:|
| verification scan | 35.1% |
| candidate-window maintenance | 30.0% |
| pop, unattributed | 16.2% |
| push | 7.4% |
| remove from bucket | 6.9% |
| bucket traversal | 4.3% |

**≈76% of extraction time is verification and window maintenance** — the two
components a model cannot make cheaper, because they run whatever the model
predicts. Three of those phases (push, remove, traversal) sit at or below the
instrument's measured 35.9-tick probe floor and are reported as untrustworthy
rather than quoted.

Two suspects were ruled out by measurement rather than argument:

- **Allocation.** Counted exactly via a replaced global `operator new`: 900
  allocations for the learned queue and 900 for the binary heap, identical to
  the byte, all of them Dijkstra's per-query result vectors. **The queue
  allocates nothing in steady state.**
- **Comparison count.** See §5.

## 4. The learned component

Trained on labels from the *trusted reference search*, never from the learned
queue — a model fitted to the queue's own behaviour would be rewarded for
reproducing its mistakes.

| | `snap_facebook` | `dimacs_ny` |
|---|---:|---:|
| held-out ROC AUC | **0.7909** | **0.5337** |
| precision lift over chance | **22.25×** | **1.12×** |

**Static node features carry real signal on a social graph and almost none on a
road network.** The reason is structural: road networks have near-uniform degree
≈ 3, so degree-based features cannot separate 264,346 nodes. Held-out and
in-sample scores agree to within 0.002 AUC, so this is not overfitting — the
road-network model does not fit its training split either.

This is the same structural divide seen from the other side: the learned queue
examines 13.2 entries per extraction on `dimacs_ny` against 225.9 on
`snap_facebook`. **The low uniform degree that makes the candidate window work
on road networks is the same property that leaves a static feature set with
nothing to learn from.**

## 5. Three results about methodology

These generalise beyond this system and are stated separately because they are
reusable.

**Comparison count does not predict runtime.** Measured three independent ways:
the learned queue performs 12.9% *fewer* comparisons than a binary heap while
running 4.6× slower; bucketing removes two thirds of the comparisons and costs
time; the trained model performs 0.09% *more* comparisons and is faster. Any
evaluation of a priority queue that reports comparison counts as a proxy for
speed is reporting something else.

**Matrix position inflated an effect by 45%.** Running configurations in a fixed
order within one process is the right design for pairing, but it confounds
position with treatment. Rerunning the ladder in reverse halved the measured
effect without changing its sign. No result here should be quoted for magnitude
unless its order has been controlled.

**Data leakage inflated an effect by ~32%.** The model was initially trained on
labels derived from the same queries it was then benchmarked on — forbidden by
this project's own specification and unnoticed until the spec was reopened.
Retraining on a query set with zero overlap reduced the measured speedup from
2.10 ms to 1.43 ms. **The leak did not create the effect, but it did inflate
it**, and it would have been invisible in the reported numbers.

## 6. What this does not show

- **Two datasets measured end to end**, one road network and one social graph.
  Four further prepared graphs were not measured.
- **No hardware counters anywhere.** The development host runs under WSL2, which
  exposes no CPU PMU. Cycles, cache misses and branch misses are *not measured*
  and nothing is inferred to fill that gap. The hypothesis that the substrate's
  memory behaviour explains the per-extraction cost is **untested**.
- **No mechanism for the model's benefit.** It does more comparisons and less
  work; what it buys was not measured.
- **One model family** (logistic regression on six static features). The MLP and
  tree-ensemble predictors are implemented and untested against this task.
- **Dynamic features unevaluated.** Only static per-node features can be
  precomputed outside the timed loop; a model using queue or search-state
  features is a different design with a different cost and was not built.
- **Approximate mode measured in comparisons only**, not wall-clock.
- **A* not measured** with the learned queue.
- **Adaptive controller not reachable** from the benchmark runner.

## 7. Honest positioning

The specification asked for a system positioned as a *practical, adaptive,
verification-aware learning-augmented priority queue*. On the evidence collected
here, the defensible claim is narrower and mostly negative:

> A verification-aware learned priority queue can be made **correct** and can be
> made to **use a genuinely predictive model**, and on a road network it is still
> several times slower than a binary heap — because the verification machinery
> that makes prediction safe costs an order of magnitude more than prediction
> saves, and because the bucket substrate it is built on had already captured
> most of the available advantage classically.

That is a useful result. It identifies precisely where the cost lives (§3), why
a better model cannot fix it (§2, §4), and it supplies three reusable
methodological cautions (§5). It does not support the claim that a learned
priority queue beats strong classical baselines on these workloads.

**The most promising direction the evidence suggests is not a better model.** It
is making verification cheaper, or avoiding it — because at ≈76% of extraction
time and slower on 100% of queries, verification is the whole deficit.

## 8. Reproducing

```bash
scripts/reproduce/build_all.sh
scripts/reproduce/run_main_experiments.sh
scripts/reproduce/generate_tables.sh
scripts/reproduce/generate_figures.sh
```

Comparison counts reproduce to the unit and are checked automatically against
the values recorded here; the run fails if they differ. Timings will not match
across machines and are not expected to. See `docs/reproducing.md`.

Supporting records, each with its own protocol and limitations:
`docs/experiments/` — datasets, baseline measurements, phase profile,
approximate mode, static-scorer training, ablation waterfall, and the
profiling-availability note.
