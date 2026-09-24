# A\* with the learned queue

ESD.md §39 milestone 11 is A\*, and it was implemented early. It had never been
measured, for a reason that only surfaced when this measurement was attempted:
**A\* was not reachable from a saved graph at all.**

## The defect that hid this experiment

The binary graph format stored a coordinate *flag* rather than a coordinate
*system*. `read_binary_graph` restored the coordinate array but left the system
at `kNone`, and `CsrGraph::has_coordinates()` requires both. Every graph loaded
from disk therefore reported having no coordinates, and `lpq_benchmark` rejected
every heuristic except `zero` — which reduces A\* to Dijkstra.

826 tests passed throughout. The round-trip test asserted that the coordinate
*array* survived the round trip, and it did; it never asked the loaded *graph*
whether it could use it. Every A\* unit test built its graph in memory, where
the system is set directly and the bug does not exist.

Fixed by storing the system (format version 3). Version 2 files still load, so
no dataset was re-converted. Five regression tests now assert on the loaded
graph rather than on the loaded array.

## The result

`dimacs_ny`, 100 single-target queries, euclidean heuristic, 11 repetitions,
single core. Comparison counts are exact; the Dijkstra column is the same
manifest measured by the ablation.

| queue | Dijkstra | A\* (euclidean) | A\*/Dijkstra |
|---|---:|---:|---:|
| `binary_heap` | 203,801,439 | 93,314,351 | **0.458×** |
| `radix_heap` | 71,839,857 | 32,916,478 | **0.458×** |
| `lpq` | 177,580,603 | 119,548,206 | **0.673×** |

A\* works: it removes 54% of the comparisons from both classical queues. The
heuristic is doing exactly what it should.

**It helps the learned queue less than it helps everything else** — 33% against
54% — and the machinery's isolated cost therefore rises:

| | `lpq / radix_heap` |
|---|---:|
| Dijkstra | 2.472× |
| A\* | **3.632×** |

The direction is consistent with the mechanism the generalisation sweep
identified. A\* prunes the search by settling fewer nodes, but the surviving
frontier is concentrated in a narrower band of keys, so buckets hold entries
that are closer together and the candidate window covers proportionally less of
each one. The machinery is charged per extraction; A\* removes extractions from
everyone and makes the remaining ones relatively worse for `lpq`.

This is an interpretation of one measurement, not a tested claim. The falsified
bucket-width hypothesis is a standing reminder of what that distinction costs.

## What this does not show

- **One heuristic on one graph.** `manhattan` and `haversine` are implemented
  and were not run. `dimacs_ny` coordinates are geographic, so `haversine` is the
  metrically correct choice and `euclidean` is the conventional benchmark one.
- **No wall-clock claim.** The A\* rows come from a single invocation, so the
  queues can be compared against each other, but not against the Dijkstra
  numbers, which come from a different one. Cross-invocation timing on this host
  varies by up to 2.3× on identical work.
- **Admissibility is not proven here, it is checked per query.** The runner
  validates every result against `reference_dijkstra_heap`; an inadmissible
  heuristic would surface as a wrong distance rather than as a silent
  approximation.

## Reproducing

```bash
lpq_benchmark --dataset datasets/processed/dimacs_ny.lpqgraph \
  --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
  --algorithm astar --heuristic euclidean \
  --pq all --repetitions 11 --warmup 3 --require-pinning \
  --output results/raw/dimacs_ny_astar_euclidean_v1.json
```
