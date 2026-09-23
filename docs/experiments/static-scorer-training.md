# Training the static node scorer: it works on a social graph and not on a road network

The first measurement of the project's learned component. Until now every
benchmark ran with a constant scorer, so nothing measured had a model in it.

The result is split. On a social graph the static features carry real,
generalising signal. On a road network — the dataset this project's headline
measurements use — they carry **almost none**.

Everything below is measured. The classification scores are held out: a node is
wholly in the training split or wholly in the test split, never both.

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

Read the accuracy row against the one below it. On both datasets the model's
accuracy is within a fraction of a percent of what a model that always answers
"no" achieves. **Accuracy is nearly uninformative on this task** and is reported
only so that it cannot be quoted elsewhere without its baseline.

The held-out and in-sample scores are within 0.002 AUC of each other on both
datasets, so nothing here is overfitting. The road network's poor score is not a
generalisation failure; the model does not fit the training split either.

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

## What this does not show

- **No speedup is claimed or measured.** These are classification scores. The
  phase profile established that the LPQ's deficit is not made of comparisons
  (it performs 12.9% fewer than the binary heap while running 4.57× slower), so
  even a perfect scorer would be improving the part that is not the problem. The
  end-to-end effect of this model on wall-clock has **not been measured**.
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
