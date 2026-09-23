# Experiment records

What has actually been measured, in the order it was measured, with the later
documents correcting the earlier ones where they disagree.

These are records, not a narrative. Each states its protocol, its host, and what
it does not show. Where a figure is exact (a comparison count, an allocation
count) it says so; where it is a measurement carrying noise it says that too.

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

**[profiling.md](profiling.md)** — why no hardware performance counters appear
anywhere in this directory. WSL2 exposes no CPU PMU, so cycles, cache misses and
branch misses are **not measured** on the development host, and no figure in this
project is inferred to fill that gap.

## Corrections

Recorded rather than quietly edited away, because a reader who saw the earlier
claim needs to know it changed.

- **`approximate-mode.md` originally claimed epsilon was inert at the runner's
  default configuration.** That was generalised from one small random graph and
  is wrong: on `dimacs_ny` at the defaults the slack removes 44% of comparisons.
  The document now carries the corrected measurements and marks the superseded
  claim.

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
