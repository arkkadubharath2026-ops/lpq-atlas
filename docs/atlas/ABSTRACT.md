# Revised abstract (spec §50)

*Draft for the final report. It describes work done and results measured; every figure is
from [experiments/paper/findings.md](../../experiments/paper/findings.md) and the stored
results it cites.*

> **RETRACTION NOTICE: parts of this draft are historical (added 2026-10-13, M2 documentation
> audit; the abstract text below is unchanged).** Two statements below are **retracted** and
> must not be quoted: the sentence beginning "The one configuration in which the learned
> machinery improves on its own baseline is the adaptive controller, which removes 34% of
> comparisons", and the closing clause "except under adaptive control". The v2 audit reports
> that the classical bucket queue alone performs 116,552,995 comparisons against the adaptive
> configuration's 116,608,480 ([v2-results.md](../experiments/v2-results.md) §3; documented,
> not re-run in this audit), so the reduction is not attributable to learning. v2 does not
> validate the claim. Likewise, the phrase "an optional adaptive controller steers the queue's
> policy from verified outcomes" describes a controller only; it must not be read as online
> *learning* running. `OnlineLearner::maybe_update` and `flush` have no call site outside
> tests, so no model weight changes in any run. The rest of the draft is unchanged by this
> notice and has not been re-verified here. See `DISCREPANCIES.md` D-02 and D-11.

---

**Dynamic Approximate Shortest Path Using a Learned Priority Queue**

We study whether a learned priority queue (LPQ) can reduce the work of single-pair
shortest-path search enough to pay for the machinery that keeps it correct. The LPQ
stores entries in a key-bucketed substrate, keeps each bucket's highest-scoring entries at
its front, and extracts a candidate only after verifying it against a valid lower bound,
widening the candidate window or falling back to a classical bucket scan when
verification fails. Scores come from a model evaluated once per node on static graph
features before the search (a static learned scorer); an optional adaptive controller
steers the queue's policy from verified outcomes. An exact mode returns optimal
distances; an ε-approximate mode returns paths within (1 + ε) of optimal, and every
returned path is checked against a trusted reference search.

We evaluate on DIMACS road networks, SNAP graphs, synthetic graphs, and OpenStreetMap
road networks extracted from pinned regional snapshots, against binary, 4-ary, pairing,
Fibonacci and radix heaps under Dijkstra and A*, using paired queries, pinned timing,
medians with MAD, paired permutation tests with Holm correction, and disjoint training and
evaluation query sets.

The results are mixed and are reported as such. On the New York road network, bucketing
alone reduces priority-queue comparisons 2.8× relative to a binary heap, while the
candidate window and verification give 2.5× of that back; the learned queue is slower
than the radix heap on every measured query. The machinery's cost varies from 1.001× to
11.1× across road networks, governed by the ratio of bucket width to mean edge weight.
Static node features predict useful candidates on a social graph (held-out ROC AUC 0.79)
but carry almost no signal on road networks (0.53), and predictive quality did not
translate into end-to-end speedup. The one configuration in which the learned machinery
improves on its own baseline is the adaptive controller, which removes 34% of
comparisons on New York for an identical search. ε-approximation reduces queue work and
makes the queue robust to a poorly chosen bucket width. Hardware cache-miss counters could
not be collected on the measurement host and are not reported.

We conclude that learned savings in extraction do not, in this design, exceed the cost of
the candidate management and verification that make them safe, except under adaptive
control; and we release LPQ Atlas, an interactive system that replays the engine's real
execution traces and makes each of these measurements reproducible.
