# Training the static node scorer: it predicts well and still does not help

> **REVISION NOTICE (2026-10-13, milestone M5-P1; the text below is retained, labelled where it
> applies to one protocol).** This document's original figures were produced by training on the
> *benchmark* queries (seed 42). That is **protocol A, pre-correction**: `ablation-waterfall.md`
> records that the training labels were later moved to a separate query set (seed 917) after the
> original was found to violate ESD.md section 30, but this document was not updated. Three
> protocols are now shown side by side ([Protocols compared](#protocols-compared)):
>
> * **A, pre-correction:** trained on the benchmark queries. The original figures; they reproduce exactly.
> * **B, pair-disjoint:** trained on a separate seed-917 set that shares no (source, target) pair with
>   the benchmark. This is what `scripts/reproduce/run_main_experiments.sh` used until M5-P1. On
>   `snap_facebook` it shares 45 source nodes with the benchmark, so it is **not** source-disjoint.
> * **C, source-disjoint (the corrected protocol, owner decision 2026-10-13):** trained on a set whose
>   sources avoid every benchmark source. Generated for `snap_facebook` by
>   `scripts/data/generate_training_queries.py`; see below for `dimacs_ny`. This is what
>   `run_main_experiments.sh` now uses, and it refuses to continue if a training set shares a source
>   with a benchmark manifest.
>
> Nothing is overwritten. Classification quality (how well the model ranks nodes) and end-to-end queue
> behaviour (comparison counts) are different measurements and are reported separately.
> See `docs/atlas/DISCREPANCIES.md` D-19.

The first measurement of the project's learned component. Until now every
benchmark ran with a constant scorer, so nothing measured had a model in it.

Two findings, and the second is the one that matters.

**The features carry signal on a social graph and almost none on a road
network.** Held-out ROC AUC is 0.79 on `snap_facebook` and 0.53 on `dimacs_ny`,
and the reason is structural: road networks have near-uniform degree, so
degree-based features cannot separate their nodes.

**Fitting the target is not the same as helping, in both directions.** The
model increases queue comparisons slightly on both datasets (+0.09% on
`dimacs_ny`, +0.68% on `snap_facebook`) — yet on `dimacs_ny` it makes the queue
measurably *faster*. See `ablation-waterfall.md`, which measures that properly:
a paired test at the full protocol, checked against matrix-position bias by
rerunning with the order reversed.

Everything below is measured. The classification scores are held out: a node is
wholly in the training split or wholly in the test split, never both. The
end-to-end numbers are comparison counts, which are exact.

## What is being predicted

`StaticNodeScorer` is the deployable form of the learned component. It runs a
model **once per node**, before any timed region, and thereafter costs one array
read per push. Its input is therefore restricted to features that are constant
per node — degree and adjacent-weight statistics — because nothing else can be
precomputed that way.

The substrate groups queued entries into buckets of equal key width, and the
scorer's job is to move the entry that will be extracted next towards the front
of its bucket. So the question the model must answer is:

> given this node's static properties, how often is it the first entry
> extracted from the bucket it lands in?

**The labels come from the trusted reference search, never from the learned
queue.** A reference Dijkstra settles nodes in nondecreasing distance order,
which is exactly the order the substrate must extract them in. Had the learned
queue produced the labels, the model would be fitted to that queue's current
behaviour — mistakes included — and improving the fit would mean reproducing
them more faithfully. As a side effect the collector needs no instrumentation
hook at all: the queue's hot path is untouched.

## Protocol

| | |
|---|---|
| Collector | `lpq_collect`, reference search, full single-source per query |
| Model | logistic regression on the six static features |
| Split | 25% of **nodes** held out, drawn from seed 20260923 |
| Seed | 20260923, explicit and preserved |
| Regularisation | L2, scikit-learn `C = 1.0` |

The split is over nodes rather than observations deliberately. Every observation
of a given node has identical features, so splitting observations would put some
of a node's rows on each side and the test score would be measuring
memorisation of nodes already seen.

## Results

### Classification, pre-correction protocol (original figures, retained)

Trained on the benchmark queries (seed 42: 1,000 queries on `snap_facebook`, 100 on `dimacs_ny`).

| | `snap_facebook` | `dimacs_ny` |
|---|---:|---:|
| nodes | 4,039 | 264,346 |
| queries | 1,000 | 100 |
| bucket width | 8 | 37 |
| observations | 4,039,000 | 26,434,600 |
| positives | 65,079 | 2,781,317 |
| positive rate | 0.0161 | 0.1052 |
| **held-out ROC AUC** | **0.7909** | **0.5331** |
| in-sample ROC AUC | 0.7917 | 0.5351 |
| held-out average precision | 0.3586 | 0.1176 |
| average precision of chance | 0.0161 | 0.1052 |
| **precision lift over chance** | **22.25×** | **1.12×** |
| accuracy | 0.9844 | 0.8948 |
| accuracy of always answering "no" | 0.9839 | 0.8948 |

### Classification, protocol B: pair-disjoint

Trained on the separate query sets `*_training_queries.yaml` (seed 917, 200 queries each), as
`scripts/reproduce/run_main_experiments.sh` did until M5-P1. Re-run on 2026-10-13.

| | `snap_facebook` | `dimacs_ny` |
|---|---:|---:|
| nodes | 4,039 | 264,346 |
| queries | 200 | 200 |
| observations | 807,800 | 52,869,200 |
| positives | 13,012 | 5,637,335 |
| positive rate | 0.0161 | 0.1066 |
| **held-out ROC AUC** | **0.8037** | **0.5337** |
| in-sample ROC AUC | 0.7909 | 0.5349 |
| held-out average precision | 0.3717 | 0.1192 |
| **precision lift over chance** | **23.08×** | **1.12×** |

### Classification, protocol C: source-disjoint (corrected protocol)

`snap_facebook`: trained on `snap_facebook_training_queries_source_disjoint.yaml` (seed 917, 200
queries), whose sources are drawn only from the 3,152 nodes that no Facebook benchmark manifest uses
as a source (887 sources excluded). Model trained with `train_from_file(seed=20260923)`; the
training set, model and report are byte-identical on a second run.

`dimacs_ny`: the seed-917 manifest of protocol B shares **no source** with `dimacs_ny_queries_100.yaml`
or `dimacs_ny_queries_first30.yaml` (checked), so for those benchmark manifests protocol B already is
source-disjoint and the column is the same. The 1,000-query `dimacs_ny_queries.yaml` shares 2 sources
with it; no number in this document uses that manifest.

| | `snap_facebook` | `dimacs_ny` (100-query manifest) |
|---|---:|---:|
| queries | 200 (194 distinct sources) | 200 |
| observations | 807,800 | 52,869,200 |
| positives | 13,018 | 5,637,335 |
| positive rate | 0.0161 | 0.1066 |
| **held-out ROC AUC** | **0.7985** | **0.5337** |
| in-sample ROC AUC | 0.7890 | 0.5349 |
| held-out average precision | 0.3693 | 0.1192 |
| **precision lift over chance** | **22.91×** | **1.12×** |
| held-out accuracy | 0.9836 | (as B) |
| accuracy of always answering "no" | 0.9839 | (as B) |

On `snap_facebook` the held-out accuracy (0.9836) is slightly *below* the always-"no" baseline
(0.9839), as the accuracy row in the original table already warned: accuracy says almost nothing on
this task. All three protocols agree in kind: a clear signal on the social graph, almost none on the
road network. The Facebook held-out AUC is 0.7909 (A), 0.8037 (B) and 0.7985 (C). The range is about
0.013, and the C-versus-B difference (0.005) is smaller than the B-versus-A one; with a single node
split and one seed per protocol this document does not say which differences are more than split
noise. None of these numbers is a statement about queue performance.

Read the accuracy row against the one below it. On both datasets the model's
accuracy is within a fraction of a percent of what a model that always answers
"no" achieves. **Accuracy is nearly uninformative on this task** and is reported
only so that it cannot be quoted elsewhere without its baseline.

The held-out and in-sample scores are within 0.002 AUC of each other on both
datasets, so nothing here is overfitting. The road network's poor score is not a
generalisation failure; the model does not fit the training split either.

## Protocols compared

| | A: pre-correction | B: pair-disjoint | C: source-disjoint (corrected) |
|---|---|---|---|
| training queries | benchmark manifests, seed 42 | `*_training_queries.yaml`, seed 917, 200 queries | `snap_facebook_training_queries_source_disjoint.yaml`, seed 917, 200 queries; `dimacs_ny`: as B |
| identical (source, target) pairs shared with the benchmark | all | **0** (checked, both datasets) | **0** |
| source nodes shared with the benchmark manifests | all | `snap_facebook`: **45** with the 1,000-query manifest (4 with `first100`, 1 with `first30`); `dimacs_ny`: 0 with the 100-query and `first30` manifests, 2 with the 1,000-query one | `snap_facebook`: **0** with all three; `dimacs_ny`: as B |
| labels | trusted reference search, full single-source per query | same | same |
| split | 25% of nodes held out, `numpy.random.default_rng(20260923).permutation` | same | same |
| model | scikit-learn `LogisticRegression`, `C=1.0`, `random_state=20260923`, six static features | same | same |
| scikit-learn / numpy / scipy / Python | 1.6.1 / 2.2.4 / 1.15.2 / 3.13.16 | same | same |

**Why source level.** The collector (`lpq_collect`) runs a full single-source search per query and the
target plays no part in the labels, so a training query whose source equals a benchmark query's source
reuses the same search whatever the target is. Protocol C therefore excludes sources, not pairs. Targets
are unconstrained. The training pool is not small: 3,152 of 4,039 nodes remain allowed (78%), and the
200 queries use 194 distinct sources against 195 in protocol B, so the experiment is about the same
size. The effect of the earlier overlap on the metrics was not isolated: C differs from B in the
specific queries drawn as well as in the exclusion.

**The protocol C manifest** is generated, not hand-edited:

```bash
python scripts/data/generate_training_queries.py \
    --benchmark experiments/manifests/snap_facebook_queries.yaml \
    --also-exclude experiments/manifests/snap_facebook_queries_first100.yaml \
    --also-exclude experiments/manifests/snap_facebook_queries_first30.yaml \
    --seed 917 --count 200 \
    --output experiments/manifests/snap_facebook_training_queries_source_disjoint.yaml
lpq_collect --dataset datasets/processed/snap_facebook.lpqgraph \
            --queries-file experiments/manifests/snap_facebook_training_queries_source_disjoint.yaml \
            --output models/training/snap_facebook_source_disjoint.lpqtrain
python -c "from lpq_tools.training.train import train_from_file; \
train_from_file('models/training/snap_facebook_source_disjoint.lpqtrain', \
  'models/training/snap_facebook_source_disjoint.lpqmodel', \
  'models/training/snap_facebook_source_disjoint.report.json', seed=20260923)"
```

The benchmark manifest is unchanged (sha256 `c495353a76f4e5abd4a1f0e1c0be18aa960022f8a5df839ba33c8716fb643b49`).
`scripts/reproduce/run_main_experiments.sh` now trains `snap_facebook` on this protocol C manifest and
`dimacs_ny` on its existing seed-917 manifest, after checking each against the benchmark manifests the
pipeline reports against (`select_training_manifest` in `scripts/reproduce/common.sh`). The protocol A and B
manifests and results are kept unchanged as historical evidence (their hashes are pinned by a test), and the
script does not reproduce them.

**Artifacts** (untracked; SHA-256, produced in the scratch worktree `m5a`):

| artifact | sha256 |
|---|---|
| `snap_facebook_training_queries_source_disjoint.yaml` (tracked) | `e0105cbe771fdb313960aff3b6d99085335d9a2a615a9515d71d582d78843c1e` |
| `snap_facebook` C `.lpqtrain` | `b3261e0b40cf4cbda48368d64a944a711586319b26032d5859f29c6a28b8cd7e` |
| `snap_facebook` C `.lpqmodel` | `f5413deb366ae47cb76c6505df4fe68eef6d73a2bb34b74cabf8d4e731b62269` |
| `snap_facebook` C `.report.json` | `b53c22ff2a15c508737d12b8d00660bb93e9e0587b6b0876289c04fa70fecf12` |
| `snap_facebook` A `.lpqtrain` | `e81ac29b3fda4de4385b8145938cba8ab4e5774827a862a71fb643815469d151` |
| `snap_facebook` A `.lpqmodel` | `eee5e7c595c2bca9c35b2afb21f0fc3f998df0c6719d22f4ea7d22346daa1726` |
| `snap_facebook` A `.report.json` | `e4c3cb5c828c1eb3e255c554ba89b52d919988c84db4f0b28a10b1a192a48386` |
| `snap_facebook` B `.lpqtrain` | `01ec343dab1e39f16aa4868cbc1434f4834bb34a9e16fff887056fec4ec50e2d` |
| `snap_facebook` B `.lpqmodel` | `e098c693b82f173411b87bf7e6c795c1f5f424fb879e8b2b705824e71f07cefb` |
| `snap_facebook` B `.report.json` | `2e01723a2d6e2098efab6d707f5536811f6e6e9419badefa7abbaf87aff0fd90` |
| `dimacs_ny` A `.lpqtrain` | `a26961e3a582d3e207ca1bd5223089409f3496278db985e758fc4b8b99d09438` |
| `dimacs_ny` A `.lpqmodel` | `179416f6d3762f294c0ddcda32f866d2ae119b34a956333d1ac637d95cc711c0` |
| `dimacs_ny` A `.report.json` | `a4a1781a916a42fbaf16b15d745971cea09598ee6aa11cc758dbce41ac0b284a` |
| `dimacs_ny` B/C `.lpqtrain` | `4c2ec37e4c3279399616e1e820d41a45b444c800cc2b99b91320fb7b303f4c1e` |
| `dimacs_ny` B/C `.lpqmodel` | `eca2fb80b30b95bf06aa173b21b4dfe5163bf4c8273658b57b5cdb3d69e1e3aa` |
| `dimacs_ny` B/C `.report.json` | `a61967a3514669005a6945b6574ed603dbdb5a454e02ae89dbce449c82e1924a` |
| `snap_facebook.lpqgraph` (checksum 27545307309881865) | `b4d931a8e253841c9f771c62fca483f8664efa29b6829afd85129a81d7319589` |
| `dimacs_ny.lpqgraph` (checksum 15161404696693572585) | `10cfb716d2be624b12530ecd49e173db98f19360d67a8d0ffa32e7ccbf687abd` |

Model and report hashes depend on the library versions above. The graphs are the committed
conversion of the pinned raw files; the checksums are the graph checksums in the model reports.

## The finding

**Static node features predict bucket order on a social graph and do not on a
road network.**

The mechanism is not mysterious. A road network has near-uniform degree — around
3 — so `out_degree` and `degree_relative_to_mean` are nearly constant across
264,346 nodes and can separate almost nothing. The social graph's degree
distribution is heavy-tailed, and degree there genuinely distinguishes nodes.

This is the same structural divide the baseline measurements found from the
other direction: the LPQ examines 13.2 entries per extraction on `dimacs_ny`
against 225.9 on `snap_facebook`. The graph property that makes the candidate
window work on road networks — low, uniform degree — is the same property that
leaves a static feature set with nothing to learn from.

**On the dataset this project's headline results use, the learned component as
currently specified has essentially nothing to contribute.** A 1.12× precision
lift will not reorder buckets usefully.

## End to end: the model does not help, even where it predicts well

The classification scores above say how well the model answers its question.
They do not say whether answering it helps. That is a separate measurement, and
it has now been made: `--pq lpq_model --model <file>` builds a `StaticNodeScorer`
once per graph, outside every timed region, and runs the identical queue with it.

Comparison counts, which are exact and machine-independent, over the benchmark query
manifests (`dimacs_ny_queries_100.yaml`, 100 queries; `snap_facebook_queries.yaml`, 1,000 queries),
one repetition, exact mode, every record checked against the reference and exact:

| | control (constant scorer) | model, A: pre-correction | change | model, B: pair-disjoint | change | model, C: source-disjoint | change |
|---|---:|---:|---:|---:|---:|---:|---:|
| `dimacs_ny` | 177,580,603 | 177,732,559 | +0.086% | 177,732,303 | +0.085% | 177,732,303 (as B) | +0.085% |
| `snap_facebook` | 798,334,612 | 803,764,660 | +0.680% | 804,286,113 | +0.745% | **804,305,590** | **+0.748%** |

The protocol A column reproduces the original figures exactly (re-run on 2026-10-13); the B and C
columns are new measurements with the models listed under
[Protocols compared](#protocols-compared). The control is the same queue (`lpq`, bucket width 8, ring
1,024, modular strategy), the same 1,000-query manifest and the same binary in every column; a second
control run gave the identical count. Pops, which also move, are 3,534,412 (control), 3,531,956 (A),
3,532,317 (B) and 3,531,802 (C) on `snap_facebook`. Timing is not reported for
these runs and no timing claim is made. These counts say nothing about classification quality, and
the classification tables above say nothing about queue behaviour.

**The model does slightly more comparison work on both datasets**, including the
one where it ranks its own target with AUC 0.79 and 22x the precision of a
random ranker (protocol A; 0.80 and 23x under protocols B and C).

More comparisons is not the same as slower, and this project has measured that
distinction three separate ways. The wall-clock consequence was measured
properly afterwards in `ablation-waterfall.md`: on `dimacs_ny` the model is
about 4.8% FASTER despite the extra comparisons, confirmed against
matrix-position bias by an order-reversed rerun. The `snap_facebook` timing
remains unresolved — the numbers below came from five repetitions with no
pairing and no order control, and do not meet that standard.

The counts differ, so the scorer is demonstrably changing queue behaviour rather
than being ignored -- the pop counts move too (3,534,412 to 3,531,956 on
facebook). It is changing it in the wrong direction.

No timing claim is made **from these runs**. The measured medians were 34.4 ms
against 31.9 ms on `dimacs_ny` and 2.15 ms against 2.62 ms on `snap_facebook`,
but the across-query MAD was of the same order as the medians, every run was
flagged noisy, and five repetitions with no pairing cannot separate that. The
comparison counts above are exact and stand on their own.

`ablation-waterfall.md` measures the `dimacs_ny` timing properly and finds a
real, small speedup. The `snap_facebook` timing has not been remeasured to that
standard and should not be quoted in either direction.

### Why good prediction does not reduce examined entries

A hypothesis, not a measurement, and labelled as such. Note that it explains the
comparison counts only; the model turns out to reduce wall-clock on `dimacs_ny`
anyway, and nothing here explains that.

The scorer is asked for **P(this node is the first extraction from its bucket)**,
marginalised over every query in the training set. The queue needs something
else: within *this* bucket at *this* moment, which of the entries present has the
smallest key. Those are different questions. A node with a high marginal
probability of winning buckets is not thereby the minimum of the particular
bucket it currently sits in, so ordering the front by that probability can
promote entries that are not the current minimum and push the true minimum out
of the examined window -- which costs a verification failure and a fallback scan.

If that is right, the defect is in what the static formulation can express, not
in the fit: no amount of accuracy on a marginal, per-node, query-independent
target can order a per-instance, per-bucket comparison. Confirming it needs the
window-hit rate to be compared between the two runs, which the runner does not
currently report, so it remains untested.

## What this does not show

- **No speedup is claimed, and none was found.** The end-to-end comparison
  counts above are exact and show the model making the queue marginally worse on
  both datasets. The wall-clock effect remains **unmeasured**: at five
  repetitions the medians differed by less than the across-query MAD. A paired
  comparison at the full protocol would be needed before any timing statement.
- **The window-hit rate was not measured**, so the explanation offered above for
  why a well-fitted model does not help is a hypothesis and not a finding.
- **Only static features were used**, because only static features can be
  precomputed per node. The dynamic, queue and search-state feature groups exist
  in `FeatureVector` and are left at zero here. A model that saw them would have
  to run inside the timed loop, which is a different design with a different
  cost, and it has not been evaluated.
- **One model family.** Logistic regression. The MLP and tree-ensemble
  predictors are implemented and untested against this task.
- **Two datasets.** `dimacs_col`, `dimacs_fla`, `snap_roadnet_pa` and
  `snap_roadnet_ca` are prepared and were not collected. The road-network result
  would be far stronger if it reproduced on the other three road graphs, and
  that has not been done.
- **One bucket width per dataset**, the derived one. The labels are defined
  relative to a bucket width, so a different width is a different learning
  problem — and the approximate-mode measurements show width changes queue
  behaviour by more than an order of magnitude.

## Reproducing

The commands below reproduce the **pre-correction protocol A** (they train on the benchmark queries) and are kept
for that reason. To reproduce protocol B, use `experiments/manifests/<dataset>_training_queries.yaml` in the first
command; for the corrected protocol C, `scripts/reproduce/run_main_experiments.sh` does it (or use the
source-disjoint manifest above). For the end-to-end counts:
`lpq_benchmark --dataset <graph> --queries-file <benchmark manifest> --pq lpq_model --model <file> --repetitions 1 --warmup 0`.

```bash
lpq_collect --dataset datasets/processed/dimacs_ny.lpqgraph \
            --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
            --output models/training/dimacs_ny.lpqtrain

python -c "from lpq_tools.training.train import train_from_file, format_report; \
print(format_report(train_from_file('models/training/dimacs_ny.lpqtrain', \
  'models/exported/dimacs_ny_static.lpqmodel', seed=20260923)))"
```

The written model is loadable by the C++ engine; `python/tests/test_model_io.py`
verifies that by round-tripping a Python-written file through the real C++
loader rather than through Python.
