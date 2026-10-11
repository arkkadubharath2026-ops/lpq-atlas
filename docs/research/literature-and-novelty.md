# Literature, prior art, and what LPQ Atlas does and does not claim

Status: written during the v2 optimisation work. Every statement about a paper
below was read from the paper's text or from bibliographic records, except where
marked **unverified**. Statements about LPQ Atlas results are in
`docs/experiments/v2-results.md`, not here.

## 1. Primary base paper

Ziyad Benomar and Christian Coester, *Learning-Augmented Priority Queues*,
NeurIPS 2024 (arXiv:2406.04793; v2 November 2024). Full text read from the
arXiv PDF.

**What it proposes.** A priority queue (LAPQ) built on a skip list that uses
predictions to make insertion cheap, while `FindMin`/`ExtractMin` stay O(1).
Three prediction models:

| model | what the algorithm receives | error measure |
|---|---|---|
| dirty comparisons | a cheap, possibly wrong answer to "is u < v?" | number of wrong dirty comparisons between u and the queue, η(u, Q) |
| pointer predictions | on inserting u, a guess of u's predecessor in Q | rank distance between the guess and the true predecessor, η⃗(u, Q) |
| rank predictions | on inserting u, a guess of u's rank among all N keys that will ever be inserted | absolute rank error, η∆(u) |

**Guarantees (as stated in the paper).** Insertion in expected O(log η⃗) time and
comparisons with pointer predictions; O(log n) time but O(log η) *clean*
comparisons with dirty comparisons; O(log log N + log max η∆) amortised time and
O(log max η∆) comparisons with rank predictions (using an auxiliary van Emde
Boas tree). A matching lower bound is proved (Theorem 4.1). For Dijkstra with
rank predictions the paper states a running time of
O(m log log n + m log max|R(dᵢ) − R̂(dᵢ)|), never worse than the O(m log n) of a
binary heap.

**Experiments.** Sorting and Dijkstra, each point averaged over 30 runs, compared
with binary and Fibonacci heaps and (for sorting) the algorithms of Bai and
Coester (2023). Dijkstra is run on OSMnx city maps and Poisson-Voronoi
synthetic graphs. Predictions come either from node ranks perturbed in a
"class" or "decay" model, or from *key-rank predictions*: the keys inserted
during a Dijkstra run from a random source are memorised, and in later runs a
key is predicted to have the rank it would have among the memorised keys.
**The reported quantity is the number of comparisons.** The text read does not
report wall-clock time, and the baselines are binary and Fibonacci heaps; there
is no radix heap, bucket queue or other integer-key structure.

**Limitations relevant here.** (a) Comparison counts are not latency, and for
integer keys the strongest practical competitors (radix heaps, bucket queues)
do not compare keys at all. (b) The theory is for comparison-based queues; an
integer-key queue is outside its model. (c) The skip-list constant factors are
not benchmarked against cache-resident integer structures.

## 2. Supporting foundations

* R. K. Ahuja, K. Mehlhorn, J. B. Orlin, R. E. Tarjan, *Faster Algorithms for
  the Shortest Path Problem*, Journal of the ACM 37(2):213–223, April 1990,
  doi:10.1145/77600.77615 (verified against the MIT JACM listing and a Max Planck
  bibliography record). Introduces the radix heap (called a "redistributive
  heap" in the 1988 Princeton technical report TR-154-88). It is the strongest
  classical integer-key baseline in this project and the reason a comparison
  against binary heaps alone would be misleading.
* A. V. Goldberg and C. Harrelson, *Computing the Shortest Path: A\* Search
  Meets Graph Theory*, SODA 2005 (technical report MSR-TR-2004-24). Source of the
  ALT landmark heuristic. **Page range unverified** (the lookup did not return
  it). LPQ Atlas does not implement ALT; A\* here uses a Euclidean/Haversine
  admissible, consistent heuristic only.

## 3. What LPQ Atlas reuses, modifies, and adds

| aspect | Benomar & Coester | LPQ Atlas |
|---|---|---|
| role of the prediction | speeds up *insertion* position search | a hint about which queued entry to inspect first |
| safety | clean comparisons repair wrong predictions | a *lower bound* on all other keys verifies the chosen entry, so a wrong hint costs time, never correctness |
| substrate | skip list | bucket ring with cached per-bucket bound |
| keys | any totally ordered type | non-negative integers (bucket indexing), monotone extraction |
| evaluation | comparison counts | wall-clock time (paired, interleaved) and comparison counts, reported separately |

Prior art that LPQ Atlas does **not** claim: learning-augmented priority queues
and the use of rank/position predictions (Benomar & Coester); radix heaps and
bucket queues for integer shortest paths (Ahuja et al.; Dial's algorithm is older
still); A\* with consistent heuristics.

Original engineering in this repository (claimed only to the extent the
measurements in `v2-results.md` support it): the bound-verified candidate window
over a bucket substrate; an interleaved, randomised-order paired benchmark
protocol with a replay floor that measures how much of a search is queue time;
a cache-resident pooled bucket queue; a cost-aware, held-out-graph-validated
choice of queue geometry.

## 4. What a new paper would need

1. Wall-clock results on a controlled, dedicated Linux machine with hardware
   counters (cloud and WSL2 hosts expose none, and cloud timing drifts by
   tens of percent between minutes, which this project measured).
2. More graph families and sizes than the six used here, and held-out *graphs*
   (not only held-out queries) for any learned component.
3. A comparison against the fastest published integer shortest-path queues, not
   only the ones implemented in this repository.
4. A theoretical statement of the bound-verification guarantee and its cost in
   terms of the prediction error, parallel to the paper's η measures.
5. Verification of the venue and indexing status of anything it cites. This
   repository makes no claim about Scopus indexing or IEEE publication.
