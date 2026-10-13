# Experiment records

What has actually been measured, in the order it was measured, with the later
documents correcting the earlier ones where they disagree.

These are records, not a narrative. Each states its protocol, its host, and what
it does not show. Where a figure is exact (a comparison count, an allocation
count) it says so; where it is a measurement carrying noise it says that too.

**The synthesis of everything here is
[`experiments/paper/findings.md`](../../experiments/paper/findings.md).** Read
that first if you want the conclusion; read these if you want to check it.

## The documents

**[datasets.md](datasets.md)** — the six real graphs the project measures, where
they came from, how they were converted, and how the synthesized weights for the
SNAP graphs are derived. Read this first if a number elsewhere refers to a
dataset by id.

**[lpq-baseline-measurements.md](lpq-baseline-measurements.md)** — the first
end-to-end comparison of the learned queue against the five classical baselines.
A **negative result for the central hypothesis as implemented**: on `dimacs_ny`
the LPQ performs 12.9% *fewer* comparisons than the binary heap and is 4.57×
slower. Establishes that **comparison count does not predict wall-clock on road
networks**, which is the constraint every later document works under.

**[lpq-phase-profile.md](lpq-phase-profile.md)** — where that time goes.
Verification and candidate-window maintenance are ≈76% of extraction cost; the
queue performs **zero** allocations in steady state; per extraction the LPQ costs
≈12× the binary heap while performing 14% more extractions. Also records the
instrument's own overhead (1.40×, measured) and which phases fall below its
resolution.

**[approximate-mode.md](approximate-mode.md)** — what bounded-approximate mode
buys. On `dimacs_ny` at the shipping defaults, ε = 0.1 removes 44% of queue
comparisons. The larger effect is that approximation makes the queue **robust to
a badly chosen bucket width**: across a 135× change in width, approximate mode is
nearly flat while exact mode degrades 41×. Also records the two regimes where the
slack is provably inert.

**[static-scorer-training.md](static-scorer-training.md)** — the first
measurement with a model in it. The static features rank their target well on a
social graph (held-out AUC 0.79, 22x the precision of a random ranker; these are the
pre-correction-protocol figures; 0.80 and 23x under the pair-disjoint and source-disjoint
protocols, see the revision notice in that document) and
barely above chance on a road network (0.53, 1.12x). The model does slightly
*more* comparison work on both — which, on this project's own evidence, says
nothing about speed.

**[ablation-waterfall.md](ablation-waterfall.md)** — the total attributed to its
parts, one component at a time, all four rungs in one pinned process. The
structural result: **bucketing alone wins 2.8x on comparisons, and the candidate
window and verification engine give 2.5x of it back.** The pairwise
LPQ-versus-binary-heap framing used everywhere else in this directory hides that,
because the two effects partially cancel. The window and verification also cost
24.5 ms per query and are slower on *every query measured*.

It also carries the project's first positive result for the learned component:
the trained model is about 4.8% faster than a constant scorer on `dimacs_ny`,
confirmed against matrix-position bias by rerunning the ladder in reverse order.
And a methodological finding worth heeding: **matrix position inflated that
effect by 45%** before the order control caught it.

**[generalisation.md](generalisation.md)** — the same ladder on five road
networks plus one constructed graph, and the document that **overturns this
project's longest-standing claim**, then overturns its own first explanation.
"The learned queue does 12.9% fewer comparisons than a binary heap" is a fact
about `dimacs_ny`; across six graphs the machinery's isolated cost ranges from
1.001× to 4282×. The first explanation offered — bucket width — was recorded as
a prediction before the fifth dataset finished and was **wrong by three orders
of magnitude**. The variable that survives is **bucket width ÷ mean edge
weight**, which predicted a constructed test case's 215.5× to within 1.28× where
bucket width alone was wrong by 216×. Bucketing's own advantage generalises
throughout, and never loses.

**[astar.md](astar.md)** — A\* measured with every queue, and the format defect
that had made it **unreachable from a saved graph** for the life of the project
without any test noticing. A\* removes 54% of comparisons from both classical
queues and only 33% from the learned one, so the machinery's isolated cost rises
from 2.472× to 3.632× under A\*.

**[adaptive-controller.md](adaptive-controller.md)** — the controller and online
learner running inside a real search for the first time. On `dimacs_ny` the
adaptive configuration performs **34% fewer comparisons at identical pop
counts** — the same search, a third less queue work. The first measurement in
this project where the learned machinery improves on its own baseline.

> **Retracted (historical; note added 2026-10-13, M2).** The paragraph above is kept as
> originally written. The v2 audit attributes the 34% to the controller retreating to a
> classical scan: the classical bucket queue alone performs 116,552,995 comparisons against the
> adaptive configuration's 116,608,480 ([v2-results.md](v2-results.md) §3; documented, not
> re-run). It is **not** evidence that learning helped, and v2 does not validate it. The
> "online learner" in that document only records events; `maybe_update` and `flush` have no
> call site outside tests. See `adaptive-controller.md` and `docs/atlas/DISCREPANCIES.md` D-11.
> (Update, M5: that is still true of `lpq_adaptive`. The separate `lpq_online` queue applies updates
> between queries; see [online-learning.md](online-learning.md). No benefit of learning is claimed.)

**[profiling.md](profiling.md)** — why no hardware performance counters appear
anywhere in this directory. WSL2 exposes no CPU PMU, so cycles, cache misses and
branch misses are **not measured** on the development host, and no figure in this
project is inferred to fill that gap.

## Corrections

- **The 12.9% figure was reported as a property of the design and is a property
  of one graph.** Every document here written before `generalisation.md` uses
  it that way. Measured on six graphs the learned queue does 12.9% *fewer*
  comparisons than a binary heap on `dimacs_ny` and roughly **295 times more**
  on `snap_roadnet_ca`. The `lpq/binary` framing should not be used at all: it
  is a product of two effects that move in opposite directions and vary
  independently.

- **`generalisation.md`'s own first explanation was wrong, and was caught by its
  own pre-registration.** It attributed the spread to bucket width and recorded
  a prediction for `snap_roadnet_ca` before that dataset finished: 2.472× to
  4.304×. The measured value was **4282.402×**. The replacement — bucket width
  divided by mean edge weight — was then tested the same way on a constructed
  dataset and survived, predicting 215.5× to within 1.28×. Both the failure and
  the success are kept, in order, because the sequence is the evidence.

- **Cross-invocation wall-clock comparisons anywhere in this directory are not
  trustworthy, and now there is a number for it.** Four approximate-mode runs
  performed byte-identical work — identical comparison *and* pop counts — and
  returned medians spanning a factor of **2.3**, while each run's own
  within-query noise stayed near 2–3%. Only queues compared inside a single
  invocation may be compared on time.

Recorded rather than quietly edited away, because a reader who saw the earlier
claim needs to know it changed.

- **A\* was unmeasurable from a saved graph for the whole life of the project,
  and nothing reported it.** The binary graph format stored a coordinate
  *flag* rather than a coordinate *system*, so `read_binary_graph` restored the
  coordinate array but left the system at `kNone`. `CsrGraph::has_coordinates()`
  requires both, so every graph loaded from disk reported having no
  coordinates, and `lpq_benchmark` rejected every heuristic except `zero`. A\*
  was reachable only from a graph built in memory — which is exactly what the
  unit tests did, which is why 826 passing tests never caught it. The
  round-trip test asserted that the coordinate *array* survived; it never asked
  the loaded *graph* whether it could use it. Fixed by storing the system
  (format version 3); version 2 files still load, so no dataset needed
  re-converting. Five regression tests now assert on the loaded graph.

- **`--pq lpq_adaptive` was documented in `--help` but not dispatched.** The
  queue alias, the observer attachment and the per-repetition controller reset
  all existed; the branch that selects them did not, so the runner rejected the
  value it advertised. Any earlier statement that the adaptive configuration
  was "wired up" described code that could not be run.

- **`approximate-mode.md` originally claimed epsilon was inert at the runner's
  default configuration.** That was generalised from one small random graph and
  is wrong: on `dimacs_ny` at the defaults the slack removes 44% of comparisons.
  The document now carries the corrected measurements and marks the superseded
  claim.

- **The learned model was trained on the queries it was then benchmarked on.**
  ESD.md section 30 forbids that, and it went unnoticed through the whole
  training milestone. Retraining on a query set with zero overlap cut the
  measured speedup from 2.10 ms to 1.43 ms: the leak inflated the effect by
  about a third without creating it. `ablation-waterfall.md` carries both
  numbers, and the reproduction pipeline now refuses to train on the benchmark
  queries rather than merely warning about it.

- **`lpq-baseline-measurements.md` attributed the per-extraction overhead to
  three causes**, one of which — the bucket substrate's memory behaviour — was a
  conjecture. Two of the three were subsequently confirmed by measurement in
  `lpq-phase-profile.md`. The third is **still untested** and is now labelled as
  a hypothesis in both documents, because no cache or branch counter has been
  read on this host.

## Reading any of these safely

Two standing cautions apply to everything here.

**Comparison counts are not runtime.** This project measured a queue doing 12.9%
fewer comparisons while running 4.57× slower. A document reporting a reduction
in comparisons is reporting a reduction in comparisons, and says so explicitly
where it might be misread as a speedup.

**A learned model has now been measured, and it helps a little.** Most documents
here predate it and describe runs with a constant scorer, which was deliberate:
it isolates the queue's machinery from prediction quality. The measured benefit
is about 4.8% on one dataset, against a 24.5 ms per-query cost for the machinery
that prediction steers — so prediction recovers roughly 6.5% of what the
candidate window and verification engine spend.

**No timing magnitude here except in `ablation-waterfall.md` has been checked
for matrix-position bias**, which was measured there to inflate an effect by
45%. Structural conclusions rest on exact comparison counts and are unaffected;
timing magnitudes elsewhere should be read as carrying that uncertainty.
