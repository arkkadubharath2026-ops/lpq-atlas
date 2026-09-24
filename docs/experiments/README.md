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
social graph (held-out AUC 0.79, 22x the precision of a random ranker) and
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

**[generalisation.md](generalisation.md)** — the same ladder on four road
networks, and the document that **overturns this project's longest-standing
claim**. "The learned queue does 12.9% fewer comparisons than a binary heap" is
a fact about `dimacs_ny`; across four graphs that figure ranges from 0.266× to
3.687×. What explains the spread is **bucket width**, not graph size — two
graphs within 2% of each other in node count differ by 11× in machinery cost.
Bucketing's own advantage does generalise, holding between 0.266× and 0.352×
throughout.

**[profiling.md](profiling.md)** — why no hardware performance counters appear
anywhere in this directory. WSL2 exposes no CPU PMU, so cycles, cache misses and
branch misses are **not measured** on the development host, and no figure in this
project is inferred to fill that gap.

## Corrections

- **The 12.9% figure was reported as a property of the design and is a property
  of one graph.** Every document here written before `generalisation.md` uses
  it that way. Measured on four road networks the learned queue does 12.9%
  *fewer* comparisons than a binary heap on `dimacs_ny` and **269% more** on
  `dimacs_fla`. The `lpq/binary` framing should not be used at all: it is a
  product of two effects that move in opposite directions and vary
  independently.

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
