# The adaptive controller, measured end to end

ESD.md §40 requires that "online updates work". Until this measurement the
controller and the online learner had never run inside a real search: both were
unit-tested against synthetic event streams, and neither had a caller outside
its own tests. Those tests establish that the components behave correctly when
fed events. They cannot establish that events ever arrive.

They now do, through `ExtractionObserver` (in the `lpq` layer, so that
`lpq → adaptation → verification → lpq` never closes) and `ControllerObserver`,
which forwards each extraction to `AdaptiveController::observe()` and
optionally to `OnlineLearner::record()`.

## The result

`dimacs_ny`, 100 queries, 11 repetitions, single core, exact mode:

| configuration | comparisons | pops |
|---|---:|---:|
| `lpq` (no controller) | 177,580,603 | 13,841,954 |
| `lpq_adaptive` (`--adaptive`) | **116,608,480** | **13,841,954** |
| ratio | **0.657×** | **1.000×** |

**The pop counts are identical to the unit.** That is the result, more than the
comparison count is. Identical pops mean the two configurations performed the
same search — the same nodes settled in the same order, the same relaxations —
so the 34% reduction is not the controller trading accuracy for speed or taking
a shortcut through the graph. It is the same algorithmic outcome reached with a
third less work inside the queue.

This is the first measurement in this project where the learned machinery
improves on its own baseline rather than costing against it. Everything the
ablation waterfall and the generalisation sweep measured was the machinery
giving back what bucketing won.

## What it does not show

- **No wall-clock claim is made.** The adaptive row's median is 49.45 ms and the
  plain `lpq` row's median came from a different invocation of the runner. The
  approximate-mode measurements establish that identical algorithmic work varies
  by up to 2.3× in wall-clock across separate invocations on this host, which is
  far larger than any effect being claimed. A timing comparison would need both
  configurations measured inside one invocation, interleaved. That has not been
  run.
- **Adaptation here happens within a query and never across queries.** The
  controller and learner are reset before every repetition, deliberately:
  adaptation is stateful, so a controller carrying state would make repetition N
  measure a different trajectory from repetition 1 and the harness would be
  averaging over configurations rather than over noise. Cross-query learning is a
  different experiment and is not measured.
- **One graph.** `dimacs_ny` has a bucket-width-to-mean-weight ratio of 0.029,
  near the favourable end of the range the generalisation sweep covers. Whether
  the controller still helps at ratios where the machinery costs 215× or 4282×
  is unmeasured, and the mechanism gives no reason to assume it transfers.
- **The reduction is not attributed.** The controller can retreat to the
  classical path, resize the candidate window, and change the expansion budget.
  Which of those produced the 34% is not separated here; that is an ablation of
  the controller itself and has not been run.
- **`--adaptive` attaches the online learner.** The row above has it enabled, so
  the measurement covers controller and learner together, not the controller
  alone.

## Correctness

`AdaptationNeverMakesTheAnswerWrong` runs 60 random graphs with
`warmup_extractions = 1` and `dwell_extractions = 1`, so the controller changes
mode as often as it can within a single search, and asserts every distance
against `reference_dijkstra`. Adaptation is a performance hint; if any mode it
can select produced a wrong distance, the hint would have become a correctness
bug. The benchmark runner also validates every query against
`reference_dijkstra_heap` before recording it.

## Reproducing

```bash
lpq_benchmark --dataset datasets/processed/dimacs_ny.lpqgraph \
  --queries-file experiments/manifests/dimacs_ny_queries_100.yaml \
  --pq lpq_adaptive --adaptive \
  --repetitions 11 --warmup 3 --require-pinning \
  --output results/raw/dimacs_ny_adaptive_v1.json
```

`--adaptive` without `--pq lpq_adaptive` is rejected rather than ignored: a flag
that is silently dropped produces a row labelled as though the learner ran when
it did not.
