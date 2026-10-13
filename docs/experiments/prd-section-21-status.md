# PRD §21 ablation study: status of every required item

Written 2026-10-13 on the candidate branch `completion/m5-candidate`. It is a **status table, not a
result**: no experiment was run to produce it, and no number in it is new. Every figure is quoted
from the document named in its row, which is where it was measured. "Met" means an experiment
that isolates the item exists and its output is committed or reproducible from the committed
pipeline. "Partly met" means something relevant was measured but the item as the PRD words it was
not isolated. "Not met" means no such experiment exists, whatever the code supports.

The **Implemented?** column is separate on purpose, because "not met" hides a real difference:
a feature that **does not exist** (item 3, confidence as a control input; item 14, perturbing
predictions) needs building before it can be tested, while one that is **implemented but
unevaluated** (items 5, 7, 9, 10) needs only an experiment or a recorded decision. The column
describes code and tests, never results; its evidence is a file path. It is the status column,
not this one, that is compared against the PRD.

The PRD's objective for the section is "to demonstrate which components actually create the
improvement". Read that way, the one thing the existing evidence does show is that on `dimacs_ny`
the improvement over a binary heap comes from bucketing, not from learning
([ablation-waterfall.md](ablation-waterfall.md)). Nothing here is a claim that learning helps.

| # | PRD §21 item | Implemented? | Status (experimental) | Evidence, and what is missing |
|---|---|---|---|---|
| 1 | Classical PQ | **Yes** (evidence: `cpp/include/lpq/pq/{binary_heap,pairing_heap,fibonacci_heap,radix_heap}.hpp`; tests `cpp/tests/pq/`) | **Met** | `binary_heap`, `radix_heap` and the pooled bucket queue are measured paired on `dimacs_ny` (`ablation-waterfall.md`: 203,801,439 and 71,839,857 comparisons) and across graphs (`v2-results.md` §4–5, `generalisation.md`). |
| 2 | Learned prediction without adaptation | **Yes** (evidence: `cpp/include/lpq/lpq/learned_priority_queue.hpp`, `models/logistic_predictor.hpp`; tests `cpp/tests/lpq/test_learned_priority_queue.cpp`) | **Partly met** | The `lpq_model` rung of the waterfall (177,732,303 comparisons against 177,580,603 for the same queue with a constant scorer). One graph (`dimacs_ny`) for the wall-clock pairing; the model there has held-out AUC 0.533. The scorer's protocol history is in `static-scorer-training.md` (D-19). |
| 3 | + confidence | **Absent as a control input.** A calibrated confidence is *produced* by the predictors, but nothing consumes it (evidence: `models/prediction.hpp`, `models/calibration.hpp` (tests `cpp/tests/models/test_calibration.cpp`); no reader in `lpq/`, `verification/`, `adaptation/` (D-14)) | **Not met** | Tested in this milestone (D-14): `Candidate::confidence` is never assigned on the LPQ path and nothing reads it, so confidence cannot affect extraction. There is nothing to ablate. PRD §12 also says the *controller* must use confidence; it uses observed rank displacement instead (D-14). `CandidateManager.ConfidenceDoesNotChangeTheWindowItsOrderOrItsAccounting` is a characterisation test of the current behaviour, not evidence of compliance. A confidence-using variant would have to be built first. |
| 4 | + candidate-window verification | **Yes** (evidence: `lpq/candidate_manager.hpp`, `verification/verification_engine.hpp`; tests `cpp/tests/lpq/test_candidate_manager.cpp`, `cpp/tests/verification/`) | **Partly met** | The waterfall's `lpq` rung adds the window and the verification engine on top of bucketing: 2.472× the comparisons of `radix_heap`'s substrate (`ablation-waterfall.md`). The window and the verification are not separated from each other, and the in-place variant is in `v2-results.md` §4 (a different queue, 30 queries). |
| 5 | + online learning | **Yes (M5)**, evaluation **not done** (evidence: `adaptation/{online_learner,online_session}.hpp`, `lpq/online_node_scorer.hpp`; tests `cpp/tests/adaptation/test_online_*.cpp`, `python/tests/test_online_benchmark.py`) | **Partly met** | Implemented and verified for correctness (M5, `online-learning.md`): a frozen `lpq_online` equals `lpq_model` query for query. The effect of updating is **not measured**: one periodic pass (Facebook, 1000 queries) is recorded as a raw observation, above the control, and is not a result. No paired multi-run comparison exists. **Online-learning performance benefit is not established** by any measurement in this repository; "partly met" records that the mechanism is implemented and verified for correctness, not that learning helps. |
| 6 | + adaptive controller | **Yes** (evidence: `adaptation/adaptive_controller.hpp`; tests `cpp/tests/adaptation/test_adaptive_controller.cpp`) | **Partly met** | `adaptive-controller.md` (comparison counts only; the 34% reduction is retracted as an attribution to learning, D-11). The controller's levers are not isolated from each other, and no interleaved wall-clock pairing was run. |
| 7 | + fallback | **Yes**, unevaluated in isolation (evidence: `verification/correction_engine.hpp`; tests `cpp/tests/verification/test_correction_engine.cpp`; counted as `fallback_scans`) | **Not met** | The fallback scan exists and is exercised by unit tests (`cpp/tests/verification/`) and appears as a rate in `approximate-mode.md`. There is no experiment that isolates its contribution. |
| 8 | Exact versus approximate modes | **Yes** (evidence: `verification/correctness_policy.hpp`; tests `cpp/tests/verification/test_approximate_mode.cpp`; epsilon in records since D-17 (`python/tests/test_approximate_epsilon.py`)) | **Partly met** | `approximate-mode.md` (comparison counts on road networks, and a wall-clock section that reports host variance larger than the effect). Not yet: runtime on paired invocations, full distributions (p50/p95/p99, max, share within ε) over several graphs and ε values. D-17 (ε missing from result files) is fixed in this milestone, so such a matrix would now be self-describing. |
| 9 | Small versus large model | **Yes**, never trained or evaluated (evidence: `models/mlp_predictor.hpp`, `models/tree_ensemble.hpp`; tests `cpp/tests/models/test_mlp.cpp`, `test_tree_ensemble.cpp`; no trained artifact) | **Not met** | The MLP and tree ensemble are implemented and unit-tested (`cpp/include/lpq/models/mlp_predictor.hpp`, `tree_ensemble.hpp`) but have no trained artifact and have not been evaluated (`PROJECT_STATUS.md`, `static-scorer-training.md` "One model family"). |
| 10 | Different feature subsets | **Mechanism only**: group masks exist, nothing uses them (evidence: `FeatureSelection` in `features/feature_vector.hpp` and `feature_extractor.hpp` (test `cpp/tests/features/test_features.cpp`); no app or training script uses it; the deployed scorer is a fixed 6 of 28 (`fill_static_features`)) | **Not met** | The deployed scorer uses 6 of the 28 features (`fill_static_features`). No experiment varies the subset. |
| 11 | In-distribution versus unseen-graph testing | **Yes** (infrastructure) (evidence: datasets and derived graphs in `datasets/metadata/`, `experiments/manifests/derived_*`; leave-one-topology-out in `python/lpq_tools/policy/evaluate.py`) | **Partly met** | The queue structure was run on five road networks (`generalisation.md`); the queue-selection policy was evaluated leave-one-topology-out (`queue-policy.md`, `v2-results.md` §6: never beat radix on a held-out topology). Weight-distribution variants exist as derived datasets. The learned **node scorer** is trained per graph and has no cross-graph evaluation that I found. |
| 12 | Prediction accuracy versus runtime | **Partly** (an analysis, not a component) (evidence: held-out metrics in model reports (`models/model_report.hpp`); counts and timings in result records) | **Partly met** | Three training protocols give different held-out quality and counts on `snap_facebook` (`static-scorer-training.md`, "Protocols compared"); AUC 0.533 on `dimacs_ny` with a 1.001× count change. A handful of points, not a sweep, and no wall-clock pairing. |
| 13 | ML overhead versus queue savings | **Yes** (evidence: `MlTimings` in `metrics/result_record.hpp`, `metrics/phase_profiler.hpp` (hardware counters not measured on this host)) | **Partly met** | `lpq-phase-profile.md` attributes queue time by phase (instrumented run; hardware counters are not measured on this host). Result records carry `ml.*` timings. The online update cost is reported but kept outside the timed clock; no end-to-end paired account (M6). |
| 14 | Sensitivity to prediction error | **Absent** (evidence: no prediction-perturbation mechanism found by inspection of `cpp/include/lpq/models/` and `python/lpq_tools/`) | **Not met** | No experiment perturbs the predictions. |

## Tally

**Met 1** (item 1) · **partly met 8** (items 2, 4, 5, 6, 8, 11, 12, 13) · **not met 5** (items 3, 7, 9, 10, 14).
Fourteen items in all.

## What this table does not say

- It says nothing about speed. "Partly met" rows are not partial successes of the hypothesis,
  only partial coverage of the experiment.
- It does not rank the missing rows by value. Items 3 and 7 depend on whether confidence and
  fallback are worth building as separate levers at all (D-14); items 9 and 10 need a decision on
  the MLP and tree models (evaluate, or record them as dropped under the simplest-model rule).
- The expensive M9 matrix (weight-distribution shift, graph modification, prediction-error
  sensitivity) was **not run** and no manifests for it were registered here.
