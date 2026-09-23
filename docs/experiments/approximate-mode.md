# Approximate mode: what the slack buys, and when it buys nothing

Bounded-approximate mode is now reachable from the benchmark runner
(`--mode approximate --epsilon <e>`, or `mode: approximate` with an `epsilon` in
an experiment manifest). This records what it was measured to do.

Everything below is a **comparison count**: an exact, deterministic, machine-
independent property of the algorithm and the query set, reproducible to the
unit. No timing appears in this document, because none has been taken — see
*What this does not show*.

> An earlier draft of this document claimed that epsilon was inert at the
> runner's default configuration. That was generalised from a single small
> random graph and is **wrong**; the measurements below supersede it.

## On a road network, the slack works at the default configuration

`dimacs_ny` (264,346 nodes / 733,846 arcs), 5 queries, derived bucket width
(37 = ceil(36,946 / 1024)), epsilon 0.1:

| candidate window | exact | ε = 0.1 | reduction |
|---|---:|---:|---:|
| 2:2 | 9,400,073 | 2,820,528 | **3.33×** |
| 2:8 | 10,679,972 | 6,358,614 | 1.68× |
| 2:32 (the runner default) | 12,561,849 | 6,988,117 | **1.80×** |

No approximation guarantee was violated on any query; the runner checks every
one against the trusted reference and aborts on a breach.

So at the shipping defaults, on the dataset this project actually measures, a
10% slack removes about 44% of the queue's comparisons.

## The bucket width matters more than the slack does

Same graph, window held at 2:2, sweeping the substrate's bucket width:

| bucket width | exact | ε = 0.1 | ratio |
|---|---:|---:|---:|
| 37 (derived) | 9,400,073 | 2,820,528 | 3.33× |
| 500 | 95,756,146 | 2,925,643 | 32.7× |
| 5,000 | 389,522,237 | 3,378,701 | 115× |

**The growing ratio is not the slack getting better. It is exact mode getting
worse.** Read the columns rather than the ratio: approximate mode goes from
2.82M to 3.38M across a 135× change in bucket width, essentially flat, while
exact mode goes from 9.4M to 389M, a factor of 41.

The interpretation this supports: a wide bucket holds many entries, a two-entry
window is a poor sample of it, and exact verification therefore refuses and
falls back to scanning the bucket. The slack lets verification accept anyway, so
the approximate queue barely notices a bucket width that is catastrophic for the
exact one. **Approximation buys robustness to a badly chosen bucket width**, and
that is a larger effect than what it buys at a well-chosen one.

## Where it genuinely is inert, and why

`random:400:0.05`, window 2:2, 10 queries:

| bucket width | exact | ε = 0.1 | ratio |
|---|---:|---:|---:|
| derived | 12,627 | 12,627 | **1.00× (inert)** |
| 5 | 20,171 | 13,472 | 1.50× |
| 37 | 79,655 | 28,728 | 2.77× |
| 500 | 642,517 | 635,438 | **1.01× (inert)** |

Inert at both ends, for two different reasons.

**At the derived width** this graph's weights are small enough that the width
rounds down to a degenerate value, and a bucket then holds entries whose keys
are all equal. `bound_for` returns the next bucket's lower edge, every key in
the current bucket is below it by construction, and so exact verification
already accepts — the test `smallest.key <= (1 + ε) * bound` is *already true at
ε = 0*, and multiplying a bound that already succeeds cannot change an outcome.
Identical counts, to the unit.

**At width 500** the entire search fits in very few buckets, a two-entry window
is a negligible sample of an enormous bucket, and the bound is so far below the
window's best candidate that a 10% slack cannot bridge the gap either. Exact and
approximate both fall back almost always.

Between those two failure modes there is a range where the slack decides
acceptances, and that is where approximate mode is worth running. It is not
monotonic in bucket width, so it cannot be tuned by pushing one knob in one
direction.

## Corroboration at the component level

`cpp/tests/verification/test_approximate_mode.cpp` sweeps epsilon over 120
random graphs per setting with a two-entry window and bucket widths drawn from
1–30 — inside the useful range identified above. Measured:

| epsilon | max ratio | fallback rate |
|---:|---:|---:|
| 0.01 | 1.0000 | 0.557 |
| 0.10 | 1.0137 | 0.272 |
| 0.50 | 1.1667 | 0.034 |
| 2.00 | 1.4333 | 0.0075 |

The fallback rate falls by ~74× across the sweep and the observed ratio stays
well inside `1 + epsilon` everywhere.

## Saturation

On `random:400:0.05` at bucket width 25, ε = 0.5 and ε = 3.0 produced identical
counts (14,110 each). Once the slack is wide enough to accept the window's best
candidate, more slack has nothing left to accept. An epsilon sweep should not be
expected to trade smoothly against accuracy all the way up; past a
workload-dependent point it stops buying anything while continuing to permit
error.

## What the runner checks, and what it refuses

The approximation bound is verified **by the runner against the trusted
reference**, per query, rather than taken from the queue's own report — a queue
that miscomputed its own slack would otherwise certify itself. A ratio exceeding
`1 + epsilon` aborts the run.

Three configurations are refused rather than run:

- `--mode approximate` with no epsilon, or epsilon zero: that is exact mode
  reported under a weaker guarantee.
- `--mode exact` with an epsilon: the slack would be ignored while the command
  line went on claiming an approximation that was never applied.
- A classical heap under `--mode approximate`. Binary, 4-ary, pairing, Fibonacci
  and radix heaps have no approximation mechanism; running them would produce
  exact results carrying an approximate label, which would then sit in a results
  table beside genuinely approximate rows, faster than them, inviting precisely
  the wrong conclusion. They emit an explained row instead, in the console as
  well as in the JSON.

## What this does not show

- **No wall-clock was measured.** Every number here is a comparison count. The
  phase profile (`lpq-phase-profile.md`) established that comparison count does
  **not** predict runtime for this queue — it does 12.9% fewer comparisons than
  the binary heap and is 4.57× slower — so nothing here may be read as a speedup.
  A 44% reduction in comparisons is a reduction in comparisons.
- **Five queries on NY, ten on the random graph.** Enough for exact counts to be
  meaningful, nowhere near enough for a timing claim.
- **The useful bucket-width range has been shown to exist, not mapped.** Whether
  a width is good for exact and approximate mode simultaneously is untested and
  is the obvious ablation.
- **No scorer was involved.** The runner never calls `set_scorer`, so every
  candidate score was constant. Approximation and prediction have not been
  measured together.
- **Approximation ratios were checked but not reported here.** Every query
  passed its `1 + epsilon` bound; the distribution of achieved ratios on a road
  network has not been collected.
