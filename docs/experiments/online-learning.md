# Online learning: what is wired, what is verified, what is not claimed

Status (M5, core path complete; local branch `completion/m5-online-o1-o4`): `lpq_online` runs through
`lpq_run`, `lpq_benchmark`, the API and the web UI. **No result in this document says the learner makes anything
better.** Whether an update improves a model, or a search, has not been measured.

| Feature | State |
|---|---|
| Measured learner status in every `lpq` result; five UI states; older results not trusted | implemented, tested |
| Event contract v2 (first choice, extracted node, key-based label, two examples on a miss) | implemented, tested |
| Bounded learner: ring buffer, exact accounting, rollback, halt | implemented, tested |
| `lpq_online` in `lpq_run` with explicit, vetted model; between-query updates | implemented, tested (C++ differential tests; real engine on demo, Facebook, New York) |
| `lpq_online` in `lpq_benchmark` with explicit `--evaluation online_session` | implemented, tested |
| `lpq_online` in the API (single runs and comparisons) and the run form | implemented, tested (API and unit tests); not driven in a browser |
| In-search updates (O5) | **deferred**: not required by any core feature, and refused with a clear message |
| API benchmarks of `lpq_online` | **deferred**: refused, because they cannot state the evaluation mode |
| Playwright coverage of the new UI | **not run** |
| Approximate mode for `lpq_online` in `lpq_benchmark` | **not supported** (rows are reported unsupported, as for `lpq_model`) |
| Sanitizer, GCC/Clang and Windows runs; cross-host determinism | **not run** |
| Any measurement that updates help; any timing claim | **none made** |

## What `lpq_online` is

`lpq_online` is the learned queue (`LearnedPriorityQueue`) with three differences from `lpq_model`:

1. **The scorer reads the live model.** `OnlineNodeScorer` builds a node's static feature vector
   (`fill_static_features`, the same function `StaticNodeScorer` uses) and takes the dot product with the
   current weights, so there is no cached score that an update could leave stale, and an update costs
   O(parameters), never O(nodes). Frozen, its scores equal `StaticNodeScorer`'s bit for bit (tested for every
   node). A non-finite score is replaced by zero; keys decide correctness either way.
2. **A learner records what the verifier established.** Every extraction that made a prediction yields an event
   (below). Nothing is learned from a model's own output.
3. **Updates are applied between queries.** During a search the model is constant, so the search is the one a
   frozen `lpq_model` runs. After the search, `OnlineSession::end_query()` applies the recorded events in
   bounded batches. (An opt-in in-search mode is a later stage, O5, and is refused by `lpq_run` today.)

`lpq_adaptive` is unchanged and keeps its historical meaning: its learner records events with empty features
and there is no model for it to update.

## The event contract (version 2)

The queue reports, for each extraction, which node the model ranked first (`predicted_node`, from the window
the verifier last examined) and which node was returned (`extracted_node`), each with its key. Features come
from a `FeatureSource` implemented by the same object that scores pushes.

| Case | Examples recorded |
|---|---|
| The first choice was a minimum (its key equals the extracted key) | one positive, first choice's features |
| It was strictly worse | a negative for it (weight raised after a fallback) and a positive for the extracted node |
| No prediction was made (controller retreat, empty window) | no event |

The label is decided on keys, not node identity, so a tie is not a mistake. The objective is "was this first
choice extractable at this extraction": it is **not** the offline label ("first extracted from the bucket it
lands in") that the warm-start model was trained on, and the two are not comparable. Positives are at least half
of the online stream (against about 1.6% offline), which shifts the intercept but not the ordering; online
confidence must not be read as calibrated. Results state `label_definition: online_first_choice_extractable_v1`.

## A model is checked before any query

`lpq_run --pq lpq_online` requires an explicit artifact (`--online-model`; no default). Loading proves the
container is well formed (magic, format version, feature layout version, feature count, kind). On top of that:
the weights are finite and bounded; no weight sits on a feature the static scorer does not supply; the report
that travels with the artifact (`--online-model-report`) names this graph's checksum and this run's bucket width.
A mismatch, a missing report or a missing field is refused unless the operator passes `--allow-graph-mismatch`,
`--allow-width-mismatch` or `--online-model-no-provenance`; each override is recorded in the result. A model that
passes is reported as `suitability: "not assessed"`. The engine records the artifact's FNV-1a hash; the SHA-256
is the caller's to record. `--online-start cold` starts from zero weights and takes no artifact.

## Bounds and recovery

The ring buffer is fixed-size and drops the oldest event when full; every event is accounted for exactly once
(applied, dropped, rolled back, discarded because disabled or halted, or pending), and `accounting_balanced` says
so. One update step applies at most `max_batch` events. A batch that leaves a parameter non-finite or beyond
`max_abs_weight` is rolled back to the exact previous parameters; after `max_rollbacks` the learner halts, stops
buffering and applying, and the model stays at its last good parameters while the search carries on.

## The result block

`lpq.learner` separates attachment, recorded events, applied updates and a **measured** weight change (hashes of
the parameters at start and end). `lpq.online_learning` is derived from it and is true only if updates were
applied and the weights differ. Results from before this block carry only the old flag, which meant "a learner
was attached"; the web page shows those as "learner attached (older result; learning not verified)".

## Verified

* C++ differential tests over random graphs (explicit seeds, directed and undirected, tied weights, both
  extraction strategies, exact and approximate): an online search equals the `lpq_model` search in distances,
  parents, every counter and every learned-queue metric, in every update mode, and every exact answer equals the
  trusted reference. Mutation checks: negating the scorer's output fails these tests.
* The same through the real `lpq_run` on real graphs (see the progress log for the run).
* Poisoned (NaN, infinite, huge) parameters give exact answers; a halted learner leaves the search and the model
  untouched.

## Using it

```bash
# one query, one round of updates after the search
lpq_run --dataset datasets/processed/snap_facebook.lpqgraph --source 1622 --target 1639 \
    --pq lpq_online --online-model models/exported/snap_facebook_static.lpqmodel \
    --online-model-report models/exported/snap_facebook_static.report.json --repetitions 0

# a stream of queries: the model is carried from one query to the next, in manifest order
lpq_benchmark --dataset datasets/processed/snap_facebook.lpqgraph \
    --queries-file experiments/manifests/snap_facebook_queries.yaml \
    --pq lpq_online --evaluation online_session --model models/exported/snap_facebook_static.lpqmodel \
    --online-model-report models/exported/snap_facebook_static.report.json
```

`--online-update-mode disabled` is the frozen control (records events, applies none): its search is the
`lpq_model` search, query for query. In `lpq_benchmark` every repetition of a query runs on the state the query
started from (updates are applied after the last repetition, and its cost is `ml.online_update_ns`, outside every
timed region), so repetitions are comparable and the trajectory does not depend on the repetition count or on
`--interleave`; each record's `learner` block carries the hashes of the weights before and after. Timings of an
adaptive row are still not comparable with a frozen row's unless the query order and the session start are the
same, which `--evaluation online_session` makes explicit but cannot enforce.

> **Note added in M6 (2026-10-13): "the `lpq_model` search" means the same results and counters, not the same time.** Measured
> on `snap_facebook` (100 queries, one invocation per mode), a frozen `lpq_online` row took 1.288 [1.258, 1.307] times the
> search time of the `lpq_model` row in the same process (periodic: 1.309 [1.288, 1.335]). The consistent explanation is that
> the online scorer computes each score on demand inside the search where the static scorer reads a precomputed float; this
> was not profiled. The update itself costs a median 0.80 ms per query in `periodic` mode and sits outside the timed samples.
> No benefit of updating is established. See `docs/experiments/m6-cost-accounting.md` section 6.

Through the API a run takes `queue: "lpq_online"`, an explicit `model_id` (or `online_start: "cold"`),
`online_update_mode`, and three override flags, all off by default (`allow_graph_mismatch`,
`allow_width_mismatch`, `accept_no_provenance`). The model must have a report next to it unless
`accept_no_provenance` is set; the result records the artifact's SHA-256 and every override used. In the run form,
choose *LPQ + online learner*; the result panel shows the starting model, how it was vetted, and the learner badge.

## Not done, not claimed

In-search updates (O5, deferred); a snapshot/restore for in-search updates (not needed for between-query updates);
API benchmarks of `lpq_online` (deferred);
any measurement of whether updates help; timing of any kind; D-15 (model artifacts embed no provenance of their
own; the report carries it and the engine records what it read).
