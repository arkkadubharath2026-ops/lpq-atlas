# Ablation waterfall: what each component actually costs

A pairwise comparison of the learned queue against a binary heap hides what is
happening, because two large effects in opposite directions partially cancel.
This attributes the total to its parts by adding one component at a time.

The ladder, each rung adding exactly one thing to the rung above:

| rung | what it adds |
|---|---|
| `binary_heap` | the classical baseline: a comparison-ordered heap |
| `radix_heap` | + bucketing by key, still entirely classical |
| `lpq` | + the candidate window and verification engine, constant scorer |
| `lpq_model` | + a trained model in place of the constant scorer |

All four rungs ran in **one pinned process** over one loaded graph and one query
set, which is what makes the per-query comparisons paired.

## Protocol

| | |
|---|---|
| Dataset | `dimacs_ny`, 264,346 nodes / 733,846 arcs |
| Queries | `dimacs_ny_queries_100.yaml` |
| Repetitions | 11 measured, 3 discarded warm-up |
| Pinning | single core, enforced |
| Model | `dimacs_ny_static.lpqmodel`, held-out AUC 0.533 |
| Statistics | paired permutation test, 10,000 resamples, seed 20260923, Holm-corrected |
| Records | 400 |

## Comparison counts — exact

These are deterministic properties of the algorithm and the query set. They
reproduced to the unit across two independent runs in different orders.

| rung | comparisons | vs previous | vs base |
|---|---:|---:|---:|
| `binary_heap` | 203,801,439 | 1.000× | 1.000× |
| `radix_heap` | 71,839,857 | **0.352×** | 0.352× |
| `lpq` | 177,580,603 | **2.472×** | 0.871× |
| `lpq_model` | 177,732,303 | 1.001× | 0.872× |

**Bucketing alone achieves 2.8× fewer comparisons than the binary heap. Adding
the candidate window and verification engine gives 2.5× of that straight back.**

This is the finding the pairwise framing concealed. Against the binary heap the
learned queue looks mildly favourable on comparisons (0.871×, i.e. 12.9% fewer),
and every earlier document in this directory reports it that way. The waterfall
shows that number is the sum of a large win from bucketing and a large loss from
the learned machinery layered on top of it. The learned queue is not beating a
binary heap by a little; it is **squandering most of a 2.8× advantage its own
substrate had already won**.

## Wall-clock — paired

| step | median difference | 95% CI | B faster on | adjusted p |
|---|---:|---|---:|---:|
| `binary_heap` → `radix_heap` | −1,362,800 ns | [−2,070,900, −893,800] | 8% | 3.0e−04 |
| `radix_heap` → `lpq` | **−24,533,700 ns** | [−35,204,400, −18,621,200] | **0%** | 3.0e−04 |
| `lpq` → `lpq_model` | +2,956,350 ns | [+1,931,500, +4,188,300] | 86% | 3.0e−04 |

A negative difference means the added component made things slower.

**The window and verification engine are the deficit.** They cost ~24.5 ms per
query on top of the bucket queue and are slower on **every single query
measured**. That is consistent with the phase profile, which attributed ≈76% of
extraction time to those same two phases by direct measurement.

Bucketing itself costs time (~1.4 ms) despite removing two thirds of the
comparisons — the same comparisons-are-not-runtime result this project has now
measured three separate ways.

## The model helps, by a little, and the first estimate was inflated

The `lpq` → `lpq_model` row above says the trained model is ~3 ms faster. But
`lpq_model` ran **last** in a fixed matrix order, so position and treatment were
confounded — on a thermally variable laptop that is a real risk, not a
formality. The ladder was rerun with the order reversed:

| order | median difference | 95% CI | model faster on |
|---|---:|---|---:|
| `lpq_model` last | +2,956,350 ns | [+1,931,500, +4,188,300] | 86% |
| **`lpq_model` first** | **+1,606,650 ns** | **[+593,900, +2,625,200]** | **74%** |

**The effect survives reversal** — both confidence intervals exclude zero, on a
majority of queries either way — **and the magnitude halves.** Matrix position
inflated the first estimate by roughly 45%.

Taking the conservative estimate, the trained model makes the learned queue
about **4.8% faster** (1.6 ms against a 33.6 ms median) while performing
**0.09% more comparisons**.

That is the first positive result for the learned component in this project. It
is also small, and it is dwarfed by the 24.5 ms the machinery it steers costs in
the first place: prediction recovers about 6.5% of what the window and
verification engine spend.

It is worth noting what kind of result it is. The model does *more* comparisons
and *less* work. Whatever it is buying is not a reduction in examined entries,
and this project has no measurement of what it is — a mechanism is not offered
here because none was measured.

## The model was trained on the test queries, and the result survives anyway

The measurements above were made with a model whose training labels came from
**the same 100 queries the ablation benchmarks**. ESD.md section 30 forbids
exactly that -- "never allow test-query outcomes to influence model parameters"
-- and it went unnoticed until the specification was reopened to write up the
findings.

It was corrected rather than annotated. A training query set was generated with
seed 917 against the same graph, verified to share **zero** queries with the
benchmark set at seed 42, and the model retrained from scratch on it.

| training queries | median difference | 95% CI | model faster on |
|---|---:|---|---:|
| leaked (identical to the test set) | +2,099,550 ns | [+892,200, +2,759,850] | 84% |
| **disjoint (seed 917, zero overlap)** | **+1,427,450 ns** | **[+832,200, +2,128,800]** | **80%** |

**Leakage inflated the effect by about a third and did not create it.** Both
intervals exclude zero, on a large majority of queries either way. The
comparison counts differ slightly too (177,732,303 against 177,732,559),
confirming the retrained model is genuinely a different one rather than the same
weights under another name.

The classification scores barely moved: held-out AUC 0.5337 disjoint against
0.5331 leaked, precision lift 1.12x both. That is expected rather than
fortunate. The train/test split was already over *nodes* and the features are
static per node, so overlapping queries could not inflate a per-node ranking
score by much. The quantity leakage could plausibly have manufactured was the
wall-clock effect -- the scorer being tuned on the exact queries it was then
timed on -- and that is the one that was rechecked.

Two caveats on the comparison. The disjoint training set is twice the size (200
queries, 52.9M observations against 26.4M), so this is not a like-for-like swap
of the same quantity of data. And the leaked figure here (2.10 ms) differs from
the 2.96 ms reported earlier in this document from an identical configuration,
which is ordinary run-to-run variation and a reminder that the magnitudes here
are soft even when the direction is not.

**Best current estimate: the trained model is roughly 4% faster than a constant
scorer, on about 80% of queries, with the magnitude varying by matrix position
and by run.**

## A methodological finding

**Matrix position inflated a measured effect by 45%.** Every multi-configuration
result this project has produced ran its configurations in a fixed order in one
process, which is the right design for pairing but leaves position confounded
with treatment. The effect here was large enough to change an estimate
substantially while never changing its sign.

No earlier document in this directory controlled for order. The structural
conclusions are unaffected, because they rest on exact comparison counts, but
any timing magnitude reported elsewhere should be read as carrying a similar
uncertainty until it is checked the same way.

## What this does not show

- **One dataset.** `dimacs_ny` only. The `snap_facebook` behaviour is
  **unresolved**: an earlier five-repetition run without pairing or an order
  control suggested the model was slower there, and that run does not meet the
  standard applied above. It should not be quoted in either direction.
- **One model, one geometry.** The derived bucket width and the default 2:32
  window. The approximate-mode measurements show bucket width changes queue
  behaviour by more than an order of magnitude, so this ladder describes one
  point in that space.
- **No mechanism for the model's benefit.** It is measured, not explained.
- **No hardware counters.** None are available on this host; see
  `profiling.md`. Nothing here attributes anything to cache or branch behaviour.
- **Two orders, not a randomisation.** Reversal establishes the effect is not
  purely positional. It does not estimate the position effect precisely, which
  would need the ladder repeated across several randomised orders.

## Reproducing

```bash
lpq_benchmark --experiment configs/experiments/dimacs_ny_ablation_v1.yaml \
              --model models/exported/dimacs_ny_static.lpqmodel \
              --output results/raw/dimacs_ny_ablation_v1.json

python -c "from lpq_tools.evaluation.results import read_results; \
from lpq_tools.evaluation.ablation import build_waterfall, format_waterfall; \
print(format_waterfall(build_waterfall(read_results( \
  'results/raw/dimacs_ny_ablation_v1.json'))))"
```
