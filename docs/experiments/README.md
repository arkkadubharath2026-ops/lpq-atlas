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

**No learned model has been involved in any measurement yet.** Every run so far
used a constant scorer, which was deliberate — it isolates the queue's machinery
from prediction quality — but it means nothing here measures the learned
contribution the project exists to evaluate.
