# Online learning: what is wired, what is verified, what is not claimed

Status (M5, stages O1 to O4, local branch `completion/m5-online-o1-o4`): the mechanism exists for one
queue, `lpq_online`, through `lpq_run`. **No result in this document says the learner makes anything
better.** Whether an update improves a model, or a search, has not been measured.

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

## Not done, not claimed

In-search updates (O5); the benchmark's per-query snapshot/restore (O6); the API and UI for `lpq_online` (O7);
any measurement of whether updates help; timing of any kind; D-15 (model artifacts embed no provenance of their
own; the report carries it and the engine records what it read).
