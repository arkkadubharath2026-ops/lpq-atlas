# Queue-selection policy (`lpq_optimized`)

This document describes how the policy that `--pq lpq_optimized --policy <file>` reads is
trained and evaluated. It contains **no result numbers**: measured results belong in the
report JSON written next to each policy file and in the results write-up that cites it.

## What is predicted

The policy chooses a **queue geometry once per graph**, before any search: the radix heap
or a pooled bucket queue of a given width. It never changes a search in flight, and every
candidate is an exact queue, so the policy can pick a slow queue but never a wrong one.
When it picks the radix heap the result is a classical heap's speed and must not be
reported as a gain of the learned queue.

For every candidate width `w` one linear model predicts

    log( time(pooled_w) / time(radix heap) )  =  intercept_w + coef_w . x

where `x` is the 7-vector `policy_features()` computed from the graph's weight profile.
At run time `ratio_w = exp(intercept_w + coef_w . x)`; candidates whose ring would exceed
2^18 slots are skipped (`pooled_ring_for`, mirrored in `lpq_tools/policy/profile.py`); the
lowest-ratio candidate is used if `ratio < 1 - margin`, otherwise the radix heap.

Features (order and formulas are those of `cpp/include/lpq/lpq/queue_policy.hpp`; a test
fails if the Python list diverges from `kPolicyFeatureNames`):
`log2_max_w`, `log2_mean_w`, `p50_over_max`, `p99_over_max`, `cv_w`, `log2_nodes`,
`mean_degree`. The policy file records the names and the engine rejects a file whose list
differs.

## Margin

`margin` is the cost-aware part of the decision. A candidate must be predicted to beat
the radix heap by *more than* `margin` (in ratio terms) before the safe classical choice
is left. It prices model error and the one-off cost of profiling the graph. It is chosen
by cross-validation from `{0, 0.02, 0.05, 0.10}` (ties go to the larger margin) and is
stored in the policy file. With no policy file the engine uses the radix heap.

## Labels come from measurements only

Input is `lpq_benchmark --interleave --compare "radix_heap;pooled_bucket:w=1,ring=...@pooled_w1;..."`
result JSON. Interleaving measures every configuration of one query back to back in a
shuffled order, so `t_candidate / t_radix` is a **paired** per-query ratio.

* One table row per `(dataset, algorithm, candidate width)`.
* Label `y = log(geometric mean over queries of t_candidate / t_radix)`, with a seeded
  bootstrap standard error over queries.
* Only queries where **both** the candidate and the radix heap are `supported` count. A
  result file containing any record whose `correctness.exact_match` is not true is
  refused.
* Nothing is dropped silently: the report lists, per `(dataset, algorithm, width)`, the
  candidate records seen and how many were paired, unsupported (candidate or radix), or
  lacked a radix record, plus every non-candidate label that was ignored.
* Dijkstra and A* are separate (`algorithm` is part of the key). A policy file has no
  algorithm field, so one is trained per algorithm (`--algorithm`, default `dijkstra`).
  The engine doubles the ring step only when the algorithm string is exactly `astar`.
* If the same query of the same candidate appears in more than one result file, the
  replicates are averaged per query first, so they are not counted as extra queries.
* The engine currently writes an empty `dataset` field. The id is then taken from
  `--assume-dataset FILE=ID`, or inferred from `experiment_id` only when exactly one known
  dataset id appears in it; the report records which datasets were inferred.

## Training

One ridge regression per width on **standardised** features; the standardisation is folded
into `intercept` and `coef` so the C++ reader (which has no scaler) can use raw features.
The intercept is not penalised. The ridge strength (shared across widths) is chosen by an
inner cross-validation that leaves out one whole **topology** at a time, by out-of-fold
squared error of the predicted log-ratio. The margin is then chosen on the out-of-fold
predictions by the geometric-mean realised ratio of the resulting decisions, comparing
only candidates that were measured on the held-out graph. `--features` restricts the model
to a named subset (the other coefficients are written as 0).

**Small N.** With fewer than 4 graphs, or fewer than 2 topologies, training is refused. A
width measured on fewer than 4 graphs is not fitted and is reported as untrained. With so
few graphs the fitted line is weakly constrained; the report gives `n` everywhere and the
bootstrap and sign-test intervals are wide.

## Evaluation: nested leave-one-topology-out

For each graph in turn, every dataset sharing its **base topology** is removed from
training: the graph itself, its base, and all siblings derived from the same base. Real
graphs are their own base; a derived dataset declares `derived_from` in
`datasets/metadata/<id>.json` (a `.lpqgraph` `source_path` of another dataset is used
when `derived_from` is absent; datasets with identical node and arc counts are also
merged, which can only remove training data). A derived `derived_ny_*` variant therefore
never trains a model evaluated on `dimacs_ny`. `assert_no_leakage` enforces this in every
fold and the report lists each fold's training datasets. Metadata that is missing is an
error, not a guess.

On each fold the model is trained on the remaining graphs (including its own inner CV for
the ridge strength and margin), applied to the held-out profile exactly as the engine
would, and scored by the **realised** paired ratio of the chosen queue versus radix on that
graph, read from the measured rows. If the chosen candidate was not measured on the
held-out graph the fold is marked `unmeasured`, counted separately and excluded from the
realised-ratio aggregates; nothing is imputed.

Reported per held-out graph: chosen queue, realised ratio vs radix, oracle-best ratio
(best of radix and the measured candidates), regret. Aggregated: geometric-mean realised
ratio, fraction of graphs where the policy beat radix (more than 2% faster), tied (within 2%) or
lost (more than 2% slower, ratio > 1.02), over all graphs and over real (non-derived) graphs only. Paired
comparisons use the per-graph log ratio against two baselines: `always radix` and
`always the best single fixed candidate chosen on the training graphs` (lowest mean
log-ratio among widths measured on every training graph). Each comparison reports the
geometric-mean ratio, a seeded bootstrap 95% interval over graphs, win/tie/loss counts and
an exact two-sided sign test.

Because derived graphs share a topology with a real graph, the "all graphs" aggregate
weights heavily-derived topologies more; read it together with the real-graphs-only one.

## How to retrain

    python -m lpq_tools.policy.train \
        --results results/raw/policy/*.json \
        --profiles-dir datasets/processed --metadata-dir datasets/metadata \
        --out models/policy/queue_policy_v1.lpqpolicy --seed 20260923

(run with `PYTHONPATH=python`, or use `scripts/policy/train_policy.sh`). Every graph needs
its `<dataset>.lpqprofile` in the profiles directory; a missing one is an error naming the
file. The profile's node/arc counts and checksum are checked against the result records
and the dataset metadata. The command writes the policy and
`queue_policy_v1.report.json` (evaluation, data accounting and the sha256 of every input
result, profile and metadata file, the git commit and the seed) and **refuses to overwrite**
either: give a new `--out` for a new version.

Install a policy with `lpq_run --pq lpq_optimized --policy <file>`; the result's
`optimizer.resolved_queue` says which queue was actually measured.

## Limits

* Hardware counters are not used: no PMU is available on the development host
  (`docs/experiments/profiling.md`).
* The policy is trained and evaluated on whichever graphs have been measured; how well it
  transfers to graphs unlike those is exactly what the leave-one-topology-out evaluation
  estimates, with the uncertainty that few graphs imply.
* A candidate absent from the training results cannot be predicted; widths must be present
  in at least 4 graphs to be fitted.
