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
mildly favourable on this graph — 12.9% fewer comparisons — and that number is a
large win from the substrate minus a large loss from the machinery on top of it.
The learned queue is not narrowly beating a classical heap; it is squandering
most of an advantage its own substrate had already won.

**And on this graph is doing real work in that sentence.** Measured across four
road networks (§2a), the machinery's cost ranges from 1.001× to 11.141× and the
net figure against a binary heap ranges from 0.266× to 3.687×. The 12.9% is a
property of `dimacs_ny`, not of the design.

Wall-clock, paired permutation test, Holm-corrected:

| step | median difference | 95% CI | B faster on |
|---|---:|---|---:|
| `binary_heap` → `radix_heap` | −1,362,800 ns | [−2,070,900, −893,800] | 8% |
| `radix_heap` → `lpq` | **−24,533,700 ns** | [−35,204,400, −18,621,200] | **0%** |
| `lpq` → `lpq_model` | +1,427,450 ns | [+832,200, +2,128,800] | 80% |

The window and verification engine cost ≈24.5 ms per query and are slower on
**every query measured**. The trained model recovers ≈1.4 ms of that — about 6%
of what the machinery it steers spends.

## 2a. It does not generalise, and the first explanation of why was also wrong

The ladder repeated on five road networks plus one constructed graph, identical
protocol, exact counts, ordered by the variable that turned out to govern it:

| dataset | nodes | max w | mean w | bucket w | **bw / mean w** | radix/binary | **lpq/radix** |
|---|---:|---:|---:|---:|---:|---:|---:|
| `snap_roadnet_pa` | 1,088,092 | 1,000 | 500.20 | 1 | **0.002** | 0.266× | **1.001×** |
| `dimacs_ny` | 264,346 | 36,946 | 1293.30 | 37 | **0.029** | 0.352× | 2.472× |
| `dimacs_col` | 435,666 | 137,384 | 3499.81 | 135 | **0.039** | 0.324× | 4.304× |
| `dimacs_fla` | 1,070,376 | 214,013 | 2043.46 | 209 | **0.102** | 0.331× | 11.141× |
| `synth_ratio1_ny` † | 264,346 | 1,317 | 1.94 | 2 | **1.029** | 0.083× | **215.545×** |
| `snap_roadnet_ca` | 1,965,206 | 48,438 | 2.95 | 48 | **16.281** | 0.069× | **4282.402×** |

† constructed to test the hypothesis, after the five natural datasets suggested it.

**The 12.9% headline was a fact about `dimacs_ny`.** Across these graphs the
machinery's isolated cost spans 1.001× to 4282×.

### The explanation that failed, and why it is recorded

With four datasets in hand the obvious explanatory variable was **bucket
width** (`max_weight / 1024`), and the four rows then available were monotonic
in it. That claim was written down together with a prediction for the dataset
still running: `snap_roadnet_ca`, bucket width 48, should cost between
`dimacs_ny`'s 2.472× and `dimacs_col`'s 4.304×.

**It measured 4282.402×** — wrong by three orders of magnitude, at a bucket
width sitting between the two graphs it was supposed to fall between.

The pre-registration is the only reason this is a refutation rather than a
footnote. Written after the fact, the natural move would have been to add a term
and keep the theory.

### The variable that survived a test

`snap_roadnet_ca` differs in its weight *distribution*: mean edge weight **2.95**
against a maximum of **48,438**. The substrate sizes buckets from the maximum, so
its buckets are ~16× wider than a typical edge. A bucket is wide or narrow only
relative to the spacing between consecutive keys, and that spacing is set by the
typical edge weight, not the largest one.

That variable was chosen after seeing the number it explains, so it was treated
as a fit and given its own test. `synth_ratio1_ny` holds the `dimacs_ny`
topology fixed and replaces only the weights, landing at ratio 1.029 where the
two hypotheses disagree by two orders of magnitude. The predictions were
committed before the run:

| hypothesis | predicted | measured | off by |
|---|---:|---:|---:|
| bucket width | ~1.0× | 215.5× | **216×** |
| bucket width ÷ mean weight | 167.8× (band 55.9–503.3×) | 215.5× | **1.28×** |

Bucket width failed a second time, and in the opposite direction from its first
failure. A variable that errs both ways is not noisy; it is the wrong variable.

**What does generalise:** bucketing's advantage — `radix/binary` never exceeds
0.352× on any graph measured, and improves to 0.069× on the most concentrated —
and the direction of the machinery's cost, which is never below 1.0×. The window
and verifier never reduce comparisons relative to the bucket queue they sit on.

This makes the project's conclusion sharper rather than weaker. The learned
design is **viable only where a bucket is far narrower than a typical edge,
which is exactly where it also has nothing to contribute**: there every entry in
a bucket carries effectively the same key, so verification succeeds trivially
and the machinery is free because it is doing nothing.

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

> *Note added 2026-10-13 (M5-P1).* The two columns come from different protocols. The
> `snap_facebook` values are the pre-correction ones (trained on the benchmark queries, seed 42);
> the `dimacs_ny` AUC is the disjoint-protocol value (seed 917). Under the disjoint protocol
> `snap_facebook` gives 0.8037 and 23.08×. See `docs/experiments/static-scorer-training.md`
> ("Protocols compared") and `docs/atlas/DISCREPANCIES.md` D-19. The values above are unchanged.

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

### The adaptive controller is the one place the machinery pays

> **RETRACTED: historical section (note added 2026-10-13, M2 documentation audit; the text of
> this section is unchanged).** The heading and conclusion of this section are not supported.
> The v2 audit reports that the classical bucket queue alone performs **116,552,995**
> comparisons on this workload, against **116,608,480** for `lpq_adaptive`, a difference of
> 0.05% ([v2-results.md](../../docs/experiments/v2-results.md) §3; documented, not re-run in
> this audit). The 34% reduction therefore comes from the controller retreating to a
> classical scan, not from learning, and **this is not a place where the learned machinery
> pays**. v2 does not validate the claim. Also, the "online learner attached to a real search"
> only records events: `OnlineLearner::maybe_update` and `flush` have no call site outside
> tests, so no model weight changed. The same-pop-count observation below remains a true
> statement about the two runs. See `docs/atlas/DISCREPANCIES.md` D-02 and D-11.

Measured end to end on `dimacs_ny`, 100 queries, exact mode, with the controller
and online learner attached to a real search for the first time:

| configuration | comparisons | pops |
|---|---:|---:|
| `lpq` | 177,580,603 | 13,841,954 |
| `lpq_adaptive` | **116,608,480** | **13,841,954** |

**The pop counts are identical to the unit**, so the two configurations
performed the same search — same nodes settled, same order — and the 34%
reduction is not bought by taking a different path through the graph. It is the
same algorithmic outcome with a third less work inside the queue.

This is the only measurement in this project where the learned machinery
improves on its own baseline rather than giving back what bucketing won. It is
one graph, at a favourable ratio of 0.029, with no wall-clock claim attached,
and which of the controller's three levers produced it has not been separated.

### A* is implemented, and helps the learned queue least

| queue | Dijkstra | A* (euclidean) | A*/Dijkstra |
|---|---:|---:|---:|
| `binary_heap` | 203,801,439 | 93,314,351 | **0.458×** |
| `radix_heap` | 71,839,857 | 32,916,478 | **0.458×** |
| `lpq` | 177,580,603 | 119,548,206 | **0.673×** |

A* removes 54% of comparisons from both classical queues and 33% from the
learned one, so the machinery's isolated cost rises from 2.472× to **3.632×**
under A*. This experiment was impossible until a format defect was fixed: the
graph file stored a coordinate *flag* rather than a coordinate *system*, so
every graph loaded from disk reported having no coordinates and every heuristic
except `zero` was rejected. 826 tests passed throughout, because every A* test
built its graph in memory.

## 5. Four results about methodology

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

**Identical work varied 2.3× in wall-clock across invocations.** Four
approximate-mode runs at different epsilon produced byte-identical comparison
*and* pop counts, and returned medians from 26.7 ms to 61.5 ms, while each run's
own within-query noise stayed near 2-3%. Each invocation measured itself
consistently and disagreed with the others. Only configurations run inside a
single invocation may be compared on time; every cross-dataset table in this
project compares exact counts for this reason.

## 6. What this does not show

- **Five road networks, one social graph, one constructed graph.** No other
  graph class. `snap_facebook` was not part of the generalisation sweep.
- **The governing ratio is a six-point relationship with one validation point.**
  It is monotonic over four orders of magnitude and it survived a discriminating
  pre-registered test, but it is a summary of measurements rather than a model
  derived from the substrate.
- **Bucket width was not swept within a single natural graph** in the
  generalisation sweep; each dataset contributes the one width the runner
  derives. `synth_ratio1_ny` varies the weights on a fixed topology, which
  begins that sweep and is not the whole of it.
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
- **Approximate mode saturates by epsilon = 0.01** on `dimacs_ny` and its
  wall-clock column cannot be read as a speedup; where between 0 and 0.01 the
  slack stops mattering is unmeasured.
- **A\* measured with one heuristic on one graph.** `manhattan` and `haversine`
  are implemented and were not run.
- **[Superseded 2026-10-13: the 34% has since been attributed to the classical scan; see the
  retraction notice in the adaptive-controller section above.]** **The adaptive controller's
  34% is unattributed.** It has three levers and the
  measurement does not separate them, adapts within a query but never across
  queries, and was run on one graph at a favourable ratio.

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
