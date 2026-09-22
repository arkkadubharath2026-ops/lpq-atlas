# ESD — Engineering & System Design
## Dynamic Approximate Shortest Path Using a Learned Priority Queue

## 1. Engineering Objective

Implement a high-performance, research-grade C++ shortest-path engine with a modular learned priority queue, supported by Python tooling for training, experimentation, analysis, and visualization.

The architecture must make it possible to change:

- shortest-path algorithm,
- priority queue,
- prediction model,
- feature set,
- verification policy,
- adaptation policy,
- approximation policy,
- dataset,
- benchmark configuration

without rewriting the rest of the system.

## 2. Technology Stack

### Core

- C++20 or newer.
- CMake.
- GoogleTest or Catch2 for unit/integration testing.
- Google Benchmark or an equivalent controlled benchmark framework.
- STL only where appropriate; custom high-performance structures for research-critical components.

### ML / experimentation

- Python 3.11+.
- NumPy.
- pandas.
- scikit-learn.
- PyTorch only if the final selected model requires it.
- matplotlib.
- scipy/statsmodels where appropriate.

The shortest-path hot path must remain native C++.

### Tooling

- Git.
- CMake presets.
- clang-format.
- clang-tidy.
- sanitizers.
- Linux perf where available.
- Docker/Dev Container for reproducibility.

## 3. Repository Layout

```text
dynamic-lpq/
├── README.md
├── PRD.md
├── ESD.md
├── LICENSE
├── CMakeLists.txt
├── CMakePresets.json
├── Dockerfile
├── docker-compose.yml
├── .gitignore
│
├── cpp/
│   ├── include/
│   │   └── lpq/
│   │       ├── graph/
│   │       ├── algorithms/
│   │       ├── pq/
│   │       ├── lpq/
│   │       ├── features/
│   │       ├── models/
│   │       ├── verification/
│   │       ├── adaptation/
│   │       ├── metrics/
│   │       └── common/
│   ├── src/
│   │   ├── graph/
│   │   ├── algorithms/
│   │   ├── pq/
│   │   ├── lpq/
│   │   ├── features/
│   │   ├── models/
│   │   ├── verification/
│   │   ├── adaptation/
│   │   ├── metrics/
│   │   └── common/
│   └── tests/
│
├── python/
│   ├── data/
│   ├── training/
│   ├── evaluation/
│   ├── statistics/
│   ├── plotting/
│   └── utilities/
│
├── configs/
│   ├── datasets/
│   ├── models/
│   ├── experiments/
│   └── hardware/
│
├── datasets/
│   ├── raw/
│   ├── processed/
│   └── metadata/
│
├── models/
│   ├── checkpoints/
│   └── exported/
│
├── experiments/
│   ├── manifests/
│   ├── runs/
│   └── paper/
│
├── results/
│   ├── raw/
│   ├── processed/
│   ├── figures/
│   └── tables/
│
├── scripts/
│   ├── build/
│   ├── data/
│   ├── benchmark/
│   ├── profile/
│   └── reproduce/
│
└── docs/
    ├── architecture/
    ├── algorithms/
    ├── experiments/
    └── paper/
```

## 4. Architectural Principles

### 4.1 Correctness first

Every algorithm must have a trusted reference implementation.

LPQ cannot directly mutate authoritative shortest-path state in a way that makes correctness impossible to audit.

### 4.2 Prediction is a hint

The ML model proposes ordering information.

The verification layer decides whether the proposal is safe to act upon.

### 4.3 Separation of concerns

Graph storage, shortest-path logic, priority queues, prediction, verification, adaptation, and instrumentation must be independent modules.

### 4.4 Zero-cost-disabled principle

When LPQ is disabled, the system should execute the classical algorithm without unnecessary ML-related branches or allocations.

### 4.5 Reproducibility

Every experiment must be fully specified by a configuration/manifest.

## 5. Graph Representation

Implement a cache-friendly graph representation.

Preferred default:

```text
Graph
  offsets[]
  edges[]
  weights[]
```

Equivalent to a CSR-like representation.

Support:

- Directed graphs.
- Undirected graphs represented as paired directed edges.
- Non-negative integer weights.
- Non-negative floating-point weights where required.

Graph metadata must include:

- node count,
- edge count,
- directedness,
- weight type,
- minimum weight,
- maximum weight,
- mean weight,
- degree statistics,
- dataset identifier.

## 6. Priority Queue Interface

Define a common interface similar to:

```cpp
template <typename Key, typename Node>
class PriorityQueue {
public:
    virtual void push(Node node, Key key) = 0;
    virtual bool empty() const = 0;
    virtual Node top() const = 0;
    virtual std::pair<Node, Key> pop() = 0;
    virtual void decrease_key(Node node, Key key) = 0;
    virtual size_t size() const = 0;
};
```

For performance-critical production implementations, avoid virtual calls in the hot path by using templates/concepts or compile-time dispatch.

The interface is conceptual; the final implementation should optimize away abstraction overhead.

## 7. Classical Queue Implementations

Implement independently and test individually:

### Binary heap

- Array-backed.
- Position map for decrease-key where needed.
- Deterministic tie handling.

### Pairing heap

- Explicit node objects/pools.
- Decrease-key support.

### Fibonacci heap

- Correct amortized implementation.
- Carefully benchmark actual constant factors.

### Radix heap

Support only compatible monotonic integer-key conditions.

The benchmark runner must automatically mark incompatible configurations as unsupported rather than producing invalid results.

## 8. Dijkstra

Implement:

```text
dijkstra(graph, source, target?, pq)
```

Requirements:

- Non-negative edge weights.
- Early exit for single-target mode.
- Optional full SSSP mode.
- Distance array.
- Parent/predecessor array.
- Settled/finalized state.
- Instrumentation hooks.

Use lazy deletion or decrease-key consistently per queue implementation and document the choice.

## 9. A*

Implement:

```text
astar(graph, source, target, pq, heuristic)
```

Initial heuristics:

- Zero heuristic — equivalent to Dijkstra.
- Euclidean for suitable spatial graphs.
- Haversine for geographic OSM data.
- Optional Manhattan for grid graphs.

Learned heuristics are out of the primary LPQ path and should be implemented only as a separate experiment.

## 10. LPQ Internal Architecture

Recommended components:

```text
LPQ
├── AuthoritativeQueue
├── PredictionEngine
├── FeatureExtractor
├── CandidateManager
├── VerificationEngine
├── CorrectionEngine
├── AdaptationController
├── OnlineLearner
└── LPQMetrics
```

### AuthoritativeQueue

Maintains a correctness-preserving classical ordering substrate.

### PredictionEngine

Produces:

```text
Prediction {
    node
    score
    confidence
    estimated_rank
    uncertainty
}
```

### CandidateManager

Maintains a candidate set/window generated from predictions.

### VerificationEngine

Determines whether the selected candidate satisfies the active correctness policy.

### CorrectionEngine

Repairs incorrect learned ordering by:

- expanding candidate window,
- reordering,
- reinserting,
- or delegating to authoritative queue.

### AdaptationController

Selects learned/conservative/classical behavior.

### OnlineLearner

Consumes labeled events generated by verified outcomes.

## 11. Feature Extractor API

Use a fixed-size numeric feature vector to minimize dynamic allocation.

Example:

```cpp
struct SearchState {
    double current_best_key;
    uint64_t queue_size;
    uint64_t settled_nodes;
    uint64_t relaxations;
    double recent_error_rate;
};

struct NodeFeatures {
    double tentative_distance;
    double effective_key;
    double degree;
    double min_edge_weight;
    double mean_edge_weight;
    double max_edge_weight;
    double relaxation_count;
    double queue_age;
    double key_gap;
    double queue_density;
};
```

The exact feature set must be configurable.

Feature extraction must not perform graph-wide scans.

## 12. Model Interface

Define:

```cpp
class Predictor {
public:
    virtual Prediction predict(const FeatureVector&) = 0;
    virtual void update(const TrainingExample&) = 0;
};
```

Again, production hot paths should use compile-time polymorphism where possible.

Required implementations:

- Heuristic baseline.
- Linear predictor.
- Online logistic/perceptron predictor.
- Small MLP if experimentally justified.
- Optional tree-based model for offline comparison.

The predictor must support serialization.

## 13. Model Export

If a Python-trained model is selected for production:

1. Train in Python.
2. Export parameters.
3. Convert to a minimal native representation.
4. Run inference in C++.
5. Benchmark inference independently.
6. Validate C++ predictions against Python predictions.

Do not put Python interpreter calls inside Dijkstra/A*.

## 14. Online Learning Design

Every verified queue decision produces an event:

```text
PredictionEvent
├── features
├── predicted_node
├── predicted_rank
├── confidence
├── actual_rank
├── rank_error
├── was_correct
├── verification_cost
└── fallback_used
```

The online learner consumes these events.

Updates should be:

- bounded,
- deterministic where possible,
- optionally batched,
- configurable by interval.

Support:

```text
cold start
warm start
periodic update
adaptive update
disabled
```

## 15. Candidate Selection

Use an adaptive candidate window.

Initial window size can be configured as:

```text
k = max(k_min, ceil(alpha * log2(queue_size + 1)))
```

This is a starting policy, not a final research claim.

The controller should learn whether increasing `k` reduces verification cost enough to justify additional candidate evaluation.

Candidate windows must never grow without a configurable maximum.

## 16. Verification

For exact mode, verification must establish that the extracted candidate is valid under Dijkstra/A* semantics.

If verification fails:

```text
prediction failure
    ↓
expand candidate set
    ↓
re-evaluate
    ↓
if still uncertain
    ↓
authoritative classical extraction
```

Every correction must be recorded.

## 17. Approximation Controller

Approximate mode must expose:

```text
epsilon
verification_budget
max_candidate_window
fallback_policy
```

The system must distinguish:

- formal guarantee,
- empirical guarantee,
- heuristic behavior.

Do not call a heuristic "guaranteed".

## 18. Adaptive Controller State

Maintain exponentially weighted statistics:

- prediction success rate,
- mean rank error,
- p95 rank error,
- correction rate,
- average inference time,
- average feature time,
- average verification time,
- queue size.

Use hysteresis:

```text
if performance_gain > upper_threshold:
    enter learned mode

if performance_gain < lower_threshold:
    enter classical mode
```

where:

`upper_threshold > lower_threshold`

This prevents mode thrashing.

## 19. Instrumentation

Instrumentation must be compile-time configurable.

Counters:

```text
pq_pushes
pq_pops
pq_decrease_keys
pq_comparisons
lpq_predictions
lpq_correct_predictions
lpq_incorrect_predictions
lpq_candidate_checks
lpq_corrections
lpq_fallbacks
feature_extraction_ns
inference_ns
online_update_ns
verification_ns
```

Timers must use a monotonic high-resolution clock.

Do not log individual events during performance-critical benchmarks.

## 20. Hardware Profiling

Provide scripts for:

```bash
perf stat
perf record
```

Collect where supported:

- cycles,
- instructions,
- branches,
- branch misses,
- cache references,
- cache misses,
- LLC misses.

Benchmark CPU affinity should be configurable.

Record:

- CPU model,
- core count,
- RAM,
- OS,
- compiler,
- compiler version,
- optimization flags.

## 21. Benchmark Runner

Provide one unified executable:

```bash
lpq_benchmark \
  --algorithm dijkstra \
  --pq lpq \
  --dataset osm_london \
  --queries experiments/manifests/london.yaml \
  --mode exact \
  --repetitions 10 \
  --output results/raw/london_lpq.json
```

The runner must support batch execution from experiment manifests.

## 22. Experiment Manifest

Example:

```yaml
experiment: london_exact_v1
dataset: osm_london
algorithm:
  - dijkstra
  - astar
pq:
  - binary_heap
  - pairing_heap
  - radix_heap
  - fibonacci_heap
  - lpq
mode:
  - exact
query_count: 1000
repetitions: 10
seed: 42
collect:
  runtime: true
  pq_operations: true
  ml_latency: true
  hardware_counters: true
```

## 23. Result Schema

Every result record must include:

```text
experiment_id
timestamp
git_commit
dataset
graph_size
edge_count
algorithm
pq
mode
source
target
runtime_ns
pq_operations
feature_time_ns
inference_time_ns
verification_time_ns
corrections
fallbacks
path_cost
optimal_cost
approximation_ratio
hardware_metadata
```

## 24. Testing Strategy

### Unit tests

- Graph loading.
- CSR construction.
- Each PQ.
- Feature extraction.
- Predictor serialization.
- Candidate selection.
- Verification.
- Adaptation controller.

### Property tests

Priority queue invariant:

```text
pop() returns the minimum key
```

Dijkstra invariant:

```text
exact mode == trusted reference
```

A* invariant:

```text
admissible heuristic preserves optimality
```

LPQ invariant:

```text
exact mode never returns a non-optimal path
```

### Differential testing

Run the same query through:

- reference Dijkstra,
- Binary Heap,
- Pairing Heap,
- Radix Heap where valid,
- Fibonacci Heap,
- LPQ.

Compare outputs automatically.

### Fuzzing

Generate small random graphs and compare every implementation against the reference.

Include:

- duplicate edges,
- zero-weight edges,
- disconnected graphs,
- high-degree nodes,
- extremely uneven weights,
- ties,
- tiny graphs.

## 25. Numerical Policy

Use a clearly defined floating-point policy.

For integer-weight benchmarks:

- prefer integer arithmetic.

For floating-point graphs:

- use explicit epsilon comparisons.
- document tolerance.
- avoid exact binary equality for path-cost validation.

## 26. Dataset Pipeline

The pipeline should be:

```text
download
  ↓
validate
  ↓
normalize
  ↓
convert to CSR
  ↓
compute metadata
  ↓
generate query manifest
  ↓
store checksum
```

Never modify raw datasets in place.

## 27. OSM Pipeline

Use OSM-derived road graphs with:

- node coordinates,
- road connectivity,
- non-negative travel-cost weights.

Store preprocessing metadata.

Do not use geography-specific features in the model unless the experiment explicitly declares them.

## 28. Synthetic Graph Generator

Provide a unified generator:

```bash
generate_graph \
  --type random_geometric \
  --nodes 1000000 \
  --weight-distribution lognormal \
  --seed 42
```

Supported families:

- grid,
- Erdős–Rényi,
- Barabási–Albert,
- random geometric,
- Poisson Voronoi,
- road-like synthetic.

## 29. Training Pipeline

Training must be separated from evaluation.

Pipeline:

```text
graphs
  ↓
reference shortest-path runs
  ↓
prediction-event extraction
  ↓
feature dataset
  ↓
train/validation/test split
  ↓
model training
  ↓
calibration
  ↓
export
  ↓
C++ inference validation
```

The split must be graph-aware where the experiment evaluates generalization.

Do not randomly mix nodes from the same graph into both training and test sets when claiming unseen-graph generalization.

## 30. Data Leakage Prevention

Never allow test-query outcomes to influence:

- model parameters,
- feature normalization,
- calibration,
- controller thresholds.

All learned artifacts used in a test must have been produced without using test outcomes.

Online-learning experiments must clearly define what information is legitimately available before each query.

## 31. Evaluation Pipeline

```text
run benchmark
  ↓
validate output
  ↓
store raw metrics
  ↓
aggregate
  ↓
statistical analysis
  ↓
generate plots
  ↓
generate paper tables
```

Raw results must remain immutable.

## 32. Required Plots

At minimum:

1. End-to-end runtime vs graph size.
2. Speedup vs Binary Heap.
3. Speedup vs strongest classical baseline.
4. Runtime decomposition.
5. ML inference cost.
6. Feature extraction cost.
7. Queue operation counts.
8. Prediction accuracy.
9. Rank-error distribution.
10. Approximation-ratio CDF.
11. Accuracy-speed trade-off.
12. Correction rate.
13. Fallback rate.
14. Generalization across unseen graphs.
15. Distribution-shift degradation.
16. Ablation waterfall.
17. Cache misses.
18. Branch misses.

## 33. Experiment Tiers

### Tier 0 — correctness

Small graphs, exhaustive testing.

### Tier 1 — microbenchmark

Measure PQ operations and model inference.

### Tier 2 — controlled synthetic

Understand algorithm behavior.

### Tier 3 — real-world graphs

SNAP and OSM.

### Tier 4 — large-scale stress tests

Largest feasible graphs.

### Tier 5 — publication suite

Frozen configuration reproducing paper results.

## 34. Build Profiles

### Debug

```text
-O0
-g
ASan
UBSan
```

### Release

```text
-O3
-march=native
```

Do not use `-march=native` for portable release artifacts intended for other machines; provide a portable release profile too.

### Profile

Release optimizations plus instrumentation.

## 35. CI

CI must run:

- compilation,
- unit tests,
- differential tests,
- fuzz-smoke tests,
- formatting,
- static analysis.

Large benchmark suites should run separately from normal CI.

## 36. Reproducible Paper Build

Provide:

```bash
./scripts/reproduce/build_all.sh
./scripts/reproduce/run_main_experiments.sh
./scripts/reproduce/generate_figures.sh
./scripts/reproduce/generate_tables.sh
```

The scripts should fail loudly if:

- a dataset is missing,
- a checksum is wrong,
- an experiment result is incomplete,
- an expected metric is absent.

## 37. Security / Reliability

Although this is not a security product, the research code must:

- validate dataset inputs,
- avoid unsafe unchecked memory access,
- use sanitizers,
- reject malformed graph files,
- bound configuration values,
- avoid arbitrary code execution through experiment configuration.

## 38. Documentation

Documentation must include:

- architecture overview,
- algorithm descriptions,
- correctness assumptions,
- model description,
- feature definitions,
- experiment methodology,
- dataset preparation,
- reproduction guide,
- benchmark interpretation,
- known limitations.

## 39. Implementation Order

Build in this order:

1. Repository skeleton.
2. Build system.
3. Graph representation.
4. Trusted Dijkstra.
5. Binary Heap.
6. Pairing Heap.
7. Radix Heap.
8. Fibonacci Heap.
9. Differential correctness tests.
10. Benchmark infrastructure.
11. A*.
12. Instrumentation.
13. Feature extractor.
14. Predictor interface.
15. Offline model baseline.
16. LPQ candidate manager.
17. Verification engine.
18. Correction engine.
19. Exact LPQ.
20. Online learning.
21. Adaptive controller.
22. Approximate mode.
23. Dataset pipeline.
24. Synthetic generator.
25. Full benchmark suite.
26. Statistical analysis.
27. Hardware profiling.
28. Ablations.
29. Reproduction pipeline.
30. Publication artifacts.

Do not start with the neural model.

The classical system and measurement infrastructure must exist first so the ML contribution can be evaluated scientifically.

## 40. Engineering Definition of Done

The implementation is done only when:

- All modules compile independently.
- Tests pass.
- Differential tests pass.
- LPQ exact mode matches reference.
- Approximate mode reports measured approximation ratios.
- Online updates work.
- Fallback works.
- Benchmark runner produces machine-readable results.
- Plot scripts consume those results.
- Reproduction scripts work from a clean environment.
- No benchmark result is generated without hardware and git metadata.
- No performance claim can be generated without baseline comparison.

