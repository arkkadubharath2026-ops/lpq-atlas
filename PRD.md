# PRD — Dynamic Approximate Shortest Path Using a Learned Priority Queue

## 1. Product / Research Definition

### Working title

**Dynamic Approximate Shortest Path Using a Learned Priority Queue (LPQ)**

### Research objective

Build a publication-grade shortest-path research system in which a learned, adaptive priority queue accelerates Dijkstra and A* without blindly trusting ML predictions.

The system must combine:

- A high-performance classical priority-queue substrate.
- A lightweight learned predictor for extraction/order decisions.
- Confidence estimation.
- Candidate-window selection.
- Exact verification and correction.
- Online adaptation.
- Automatic fallback to classical ordering when learning is not beneficial.
- Configurable exact and bounded-approximate operating modes.
- Comprehensive instrumentation.
- Reproducible benchmarking against strong classical baselines.
- Statistical evaluation and ablation studies.

The project is not merely "Dijkstra + a neural network". The research contribution should be a robust learning-augmented queue architecture in which prediction is treated as a performance hint and correctness is controlled by an explicit verification policy.

## 2. Important Prior-Art Constraint

A 2024 NeurIPS paper, **Learning-Augmented Priority Queues** by Ziyad Benomar and Christian Coester, already establishes learning-augmented priority queues and applies rank predictions to Dijkstra. The present project MUST NOT claim that "learned priority queues" or "ML-assisted priority queues for Dijkstra" are novel by themselves.

The project's novelty must instead be established through a materially different systems/algorithmic contribution, such as:

1. An online, self-adapting predictor operating directly from search-time graph signals.
2. Confidence-aware candidate extraction rather than unconditional rank prediction.
3. Verification/correction integrated into a practical priority-queue implementation.
4. Adaptive switching between learned and classical queue behavior.
5. A unified exact + bounded-approximate operating framework.
6. Graph-distribution generalization experiments.
7. End-to-end wall-clock evaluation including feature extraction and inference, rather than only comparison counts.
8. Hardware-level analysis including cache behavior and branch behavior.
9. A principled speed/accuracy/error-budget controller.
10. Strong empirical and, where possible, formal guarantees for the proposed correction policy.

All claims of novelty must be checked against the current literature before publication.

## 3. Problem Statement

Classical Dijkstra and A* repeatedly perform priority-queue operations. In large sparse graphs, especially road networks, this can become a substantial practical cost.

The proposed LPQ learns patterns in queue states and graph-search behavior to predict which candidate is likely to have the smallest effective priority.

The predictor is not trusted as a source of correctness. It produces a ranked candidate set and confidence estimate. A verification layer checks the selected candidate against the required correctness/approximation policy.

The queue therefore follows:

Prediction → Candidate selection → Verification → Extraction → Correction/learning feedback.

## 4. Operating Modes

### 4.1 Exact mode

The returned shortest path must be identical in cost to a trusted exact reference implementation, subject only to deterministic floating-point/tie-handling policy.

ML may influence where the algorithm looks first, but no incorrect candidate may be finalized without sufficient verification.

### 4.2 Bounded approximate mode

The user specifies an error budget epsilon.

Target contract:

`cost_returned <= (1 + epsilon) * cost_optimal`

when the selected algorithmic conditions permit the guarantee.

The implementation must never silently label an empirical approximation as a formal guarantee. If a formal guarantee cannot be established for a specific mode, that mode must be documented as empirically approximate.

### 4.3 Adaptive mode

The queue dynamically chooses among:

- Learned extraction.
- Narrow verified candidate extraction.
- Wider candidate verification.
- Classical exact fallback.

The decision must depend on observed prediction quality, queue state, estimated inference cost, and the configured error/correctness policy.

## 5. Core Research Hypothesis

A lightweight learned model can predict useful local ordering information about the active shortest-path frontier sufficiently well that a verified/adaptive queue can reduce expensive queue work and improve end-to-end runtime, while retaining robust behavior when predictions degrade.

The hypothesis is considered supported only if:

- End-to-end runtime improves after including all ML overhead.
- Gains are statistically reproducible.
- The method does not depend on a single graph or source/target pair.
- Degradation under distribution shift is measured.
- Classical fallback prevents catastrophic behavior.
- Accuracy/cost trade-offs are explicitly characterized.

## 6. Algorithms

The implementation must include:

### Classical algorithms

- Dijkstra + Binary Heap
- Dijkstra + Pairing Heap
- Dijkstra + Radix Heap
- Dijkstra + Fibonacci Heap
- A* + Binary Heap
- A* + Pairing Heap
- A* + Radix Heap where weight assumptions permit it
- A* + Fibonacci Heap

### Proposed algorithms

- LPQ-Dijkstra Exact
- LPQ-Dijkstra Approximate
- LPQ-Dijkstra Adaptive
- LPQ-A* Exact
- LPQ-A* Approximate
- LPQ-A* Adaptive

A simpler LPQ baseline should also be implemented for ablation.

## 7. LPQ Architecture

The final queue should conceptually maintain:

1. A classical authoritative ordering structure.
2. A lightweight learned prediction layer.
3. A candidate index/cache.
4. A verification mechanism.
5. A correction/reinsertion mechanism.
6. Prediction-quality statistics.
7. A policy controller.

The learned component must never be the sole authority for exact-mode finalization.

### Recommended extraction flow

```text
Queue state
    |
    v
Feature extraction
    |
    v
Learned predictor
    |
    +--> predicted score/rank
    +--> confidence
    +--> uncertainty
    |
    v
Candidate window
    |
    v
Verification policy
    |
    +--> confident + cheap verification
    |          |
    |          v
    |       extract
    |
    +--> uncertain
    |          |
    |          v
    |       expand candidate set
    |
    +--> prediction failure
               |
               v
        classical fallback
```

## 8. Prediction Target

The preferred primary target is **relative ordering / extraction likelihood**, not raw distance prediction.

The predictor should estimate a score that allows the queue to rank active candidates.

The design should support two complementary targets:

1. Pairwise ordering probability:
   `P(u should precede v | state)`

2. Candidate priority score:
   `score(u | search_state)`

A direct raw-distance predictor may be implemented as an experimental ablation, but it must not be the only formulation.

## 9. Feature Design

The model should use inexpensive features derived from data already available during graph traversal.

Candidate features include:

### Node features

- Current tentative distance.
- Current effective A* key `g + h`.
- Node degree.
- In-degree/out-degree where applicable.
- Local edge-weight statistics.
- Minimum/mean/max adjacent edge weight.
- Number of successful relaxations.
- Number of failed relaxations.
- Number of queue insertions.
- Number of decreases/updates.
- Search depth / hop depth.
- Time since first discovery.

### Queue features

- Current queue size.
- Candidate's approximate queue position.
- Difference from current known minimum.
- Relative key gap.
- Local density of competing keys.
- Number of candidates inside an adaptive key window.

### Search-state features

- Current best known distance.
- Distance to target when A* is active.
- Search progress.
- Number of settled nodes.
- Relaxation rate.
- Recent prediction error.
- Recent correction rate.
- Recent model confidence calibration.

### Graph-context features

- Degree statistics.
- Local density.
- Weight distribution summary.
- Road-network spatial features when available.
- Graph family/domain identifier only if explicitly enabled as an experiment.

Feature extraction must be aggressively optimized and benchmarked.

## 10. Model Strategy

The production path should use a lightweight model suitable for CPU inference.

The system must support a model interface so multiple models can be compared:

- Constant/heuristic baseline.
- Linear model.
- Online logistic regression / perceptron-style model.
- Small gradient-boosted model where appropriate.
- Small MLP.
- Optional contextual bandit/controller experiment.

The default production candidate should be selected empirically based on:

`end-to-end runtime = queue cost + feature cost + inference cost + verification cost + graph traversal cost`

The largest model is NOT automatically the best model.

The project should prefer the simplest model that wins on the target workload.

## 11. Online Learning

The predictor must support online adaptation.

Feedback should include:

- Whether predicted candidate was actually minimum.
- Rank displacement.
- Absolute key displacement.
- Candidate-window success/failure.
- Verification expansions.
- Corrections.
- Prediction confidence versus correctness.

The system should support:

- Warm-start model.
- Cold-start model.
- Periodic updates.
- Event-triggered updates.
- Bounded update cost.

Training must never cause unbounded pauses in the shortest-path critical path.

If necessary, model updates can be batched and applied asynchronously or between queries.

## 12. Confidence and Calibration

The predictor must expose:

- Score.
- Confidence.
- Uncertainty or margin.

Confidence must be calibrated and evaluated.

Possible calibration techniques:

- Temperature scaling.
- Isotonic calibration.
- Online calibration.
- Reliability curves.

The controller must use confidence to decide how aggressively to trust predictions.

## 13. Verification Policy

Verification is a core research component.

The queue should support adaptive candidate windows.

Example:

```text
window = small
verify candidate
    |
    +-- correct --> extract
    |
    +-- incorrect --> expand window
                       |
                       +--> retry
                       |
                       +--> fallback if necessary
```

The system must measure:

- Verification comparisons.
- Candidate-window size.
- Number of corrections.
- Number of fallback operations.
- Time spent verifying.
- Prediction error distribution.

## 14. Adaptive Controller

The LPQ controller should monitor:

- Recent prediction accuracy.
- Mean and tail rank error.
- Correction frequency.
- Confidence calibration.
- Inference latency.
- Feature extraction latency.
- Queue size.
- Graph/search characteristics.

It should dynamically select a policy such as:

- Aggressive learned mode.
- Conservative learned mode.
- Hybrid mode.
- Classical mode.

The controller must have hysteresis or equivalent stabilization so it does not oscillate rapidly between modes.

## 15. Dynamic / Distribution-Shift Behavior

The system must explicitly test:

- Different source nodes.
- Different target nodes.
- Unseen graphs.
- Different graph sizes.
- Different edge-weight distributions.
- Modified graphs.
- Road closures/additions where possible.
- Synthetic distribution shifts.
- Prediction degradation.

A major research goal is graceful degradation rather than assuming stationary data.

## 16. Correctness

Exact mode must be validated against an independent trusted implementation.

For every benchmark query:

- Compare reachability.
- Compare shortest-path cost.
- Compare path length.
- Compare path validity.
- Compare predecessor/path reconstruction where deterministic comparison is meaningful.

For approximate mode:

- Compute exact optimum with the reference implementation.
- Report approximation ratio:

`returned_cost / optimal_cost`

- Report mean, median, p95, p99, maximum.
- Report percentage within each configured epsilon.

No accuracy result may be reported without the corresponding exact reference.

## 17. Datasets

### Required real datasets

- SNAP graph datasets.
- OpenStreetMap road-network datasets.

### Required synthetic datasets

Generate controlled graph families including:

- Erdős–Rényi/random graphs.
- Barabási–Albert/power-law graphs.
- Grid graphs.
- Random geometric graphs.
- Poisson Voronoi road-like graphs.
- Weight distributions:
  - Uniform.
  - Log-normal.
  - Exponential.
  - Narrow-range.
  - Heavy-tailed.

Dataset sizes must scale from small validation graphs to large stress-test graphs subject to hardware limits.

## 18. Experimental Protocol

All experiments must use controlled configurations.

For every algorithm:

- Same graph.
- Same source-target pairs.
- Same compiler settings.
- Same hardware.
- Same thread policy.
- Same graph representation.
- Same stopping criterion.
- Same path validation.

Warm-up runs must be separated from measured runs.

Use multiple repetitions and report confidence intervals.

Do not cherry-pick favorable queries.

Source-target pairs should be generated deterministically from fixed seeds and stored as experiment manifests.

## 19. Baselines

Minimum baseline matrix:

| Algorithm | Queue |
|---|---|
| Dijkstra | Binary Heap |
| Dijkstra | Pairing Heap |
| Dijkstra | Radix Heap |
| Dijkstra | Fibonacci Heap |
| A* | Binary Heap |
| A* | Pairing Heap |
| A* | Radix Heap |
| A* | Fibonacci Heap |
| Dijkstra | Proposed LPQ |
| A* | Proposed LPQ |

Also include:

- Plain array/linear scan PQ on small graphs.
- An oracle ordering experiment where useful.
- LPQ with ML disabled.
- LPQ with verification disabled only as a diagnostic, never as the correctness baseline.

## 20. Required Metrics

### End-to-end

- Wall-clock runtime.
- Throughput.
- Queries/second.
- Speedup over each baseline.

### Priority queue

- Insert operations.
- Extract-min operations.
- Decrease-key/update operations.
- Comparisons.
- Candidate inspections.
- Fallback operations.
- Corrections.
- Queue size over time.

### ML

- Feature extraction latency.
- Inference latency.
- Model update latency.
- Training overhead.
- Model memory.
- Prediction accuracy.
- Rank error.
- Confidence calibration.
- Correction rate.

### Hardware

Where available:

- CPU cycles.
- Instructions.
- Branch instructions.
- Branch misses.
- Cache references.
- Cache misses.
- LLC misses.
- Memory bandwidth.
- Peak RSS.

Use Linux perf or equivalent tooling and clearly document unavailable counters.

### Accuracy

- Exact path agreement.
- Approximation ratio.
- Mean approximation ratio.
- p50/p95/p99 ratio.
- Maximum ratio.
- Percentage within epsilon.
- Failure/correction rate.

## 21. Ablation Study

The final paper must include at least:

1. Classical PQ.
2. Learned prediction without adaptation.
3. Learned prediction + confidence.
4. + candidate-window verification.
5. + online learning.
6. + adaptive controller.
7. + fallback.
8. Exact versus approximate modes.
9. Small versus large model.
10. Different feature subsets.
11. In-distribution versus unseen-graph testing.
12. Prediction accuracy versus runtime.
13. ML overhead versus queue savings.
14. Sensitivity to prediction error.

The objective is to demonstrate which components actually create the improvement.

## 22. Statistical Rigor

For each major claim:

- Use repeated trials.
- Report mean and standard deviation or confidence interval.
- Prefer paired comparisons on identical query sets.
- Use appropriate significance tests when comparing algorithms.
- Report effect sizes, not only p-values.
- Report distributions, not only averages.

Outliers must not be silently removed.

Any exclusion must be documented before results are analyzed.

## 23. Reproducibility

The repository must contain:

- Dataset acquisition scripts.
- Dataset preprocessing scripts.
- Dataset metadata.
- Experiment configurations.
- Deterministic seeds.
- Build instructions.
- Environment specification.
- Benchmark runner.
- Metric collector.
- Plot generator.
- Statistical-analysis scripts.
- Raw result schema.
- Result validation.
- Reproduction scripts for paper figures/tables.

A single command should eventually reproduce the main paper benchmark suite, subject to hardware availability.

## 24. Performance Engineering

The implementation must:

- Use release builds for benchmarks.
- Avoid Python in the critical shortest-path loop.
- Use contiguous memory where beneficial.
- Avoid unnecessary allocations.
- Avoid virtual dispatch in hot paths where practical.
- Support compiler optimization.
- Keep ML inference on CPU for fair comparison unless a separate GPU experiment is explicitly defined.
- Separate training code from benchmark execution.
- Avoid logging inside hot loops during timed experiments.
- Support profiling builds and benchmark builds separately.

## 25. Acceptance Criteria

The project is considered technically complete only when:

1. All baseline queues are implemented and validated.
2. LPQ returns correct exact results in exact mode.
3. Approximate mode produces measured approximation ratios.
4. Adaptive fallback works.
5. Online adaptation works.
6. ML overhead is measured end-to-end.
7. Tests cover queue invariants and algorithmic correctness.
8. Results are reproducible.
9. Experiments run on both synthetic and real graphs.
10. Distribution-shift behavior is evaluated.
11. Ablations identify the contribution of every major component.
12. Performance claims are based on statistically meaningful repeated experiments.
13. The system can produce publication-quality figures/tables.
14. No novelty claim contradicts existing literature.

## 26. Research Deliverables

The repository must ultimately produce:

- Production-quality source code.
- Benchmark suite.
- Dataset pipeline.
- Experiment manifests.
- Raw results.
- Processed results.
- Publication figures.
- Publication tables.
- Complexity analysis.
- Correctness analysis.
- Ablation analysis.
- Reproducibility package.
- Technical documentation.
- Paper-ready methodology description.

## 27. Publication Positioning

The paper should position the contribution as a **practical, adaptive, verification-aware learning-augmented shortest-path priority queue**, not as the first learned priority queue.

The paper must explicitly distinguish itself from theoretical learning-augmented priority queues by emphasizing the system-level integration of online learning, confidence, verification, adaptive fallback, end-to-end runtime accounting, graph generalization, and exact/approximate operating policies.

Before submission, perform a fresh literature review covering:

- Learning-augmented algorithms.
- Learned data structures.
- Learned priority queues.
- Learned shortest-path algorithms.
- ML-guided graph search.
- Learned heuristics for A*.
- Dynamic shortest-path algorithms.
- Approximate shortest paths.
- Hardware-aware priority queues.

## 28. Non-Goals

The first version must NOT attempt to:

- Replace the entire shortest-path algorithm with a black-box neural network.
- Use a large language model in the hot path.
- Claim formal guarantees without proofs.
- Optimize solely for one benchmark.
- Optimize only ML accuracy while ignoring end-to-end runtime.
- Use GPU inference for the primary CPU comparison unless explicitly justified.
- Hide failed predictions or fallback events.
- Compare against weak or outdated baselines while omitting strong relevant methods.

## 29. Definition of Success

The strongest successful outcome is not simply:

"LPQ is faster."

It is:

"LPQ provides a measurable and reproducible end-to-end advantage over strong classical priority queues on meaningful graph workloads, while maintaining exact correctness or a clearly controlled approximation budget, adapting to changing prediction quality, degrading gracefully under distribution shift, and providing a scientifically defensible explanation of when and why the learned queue helps."

