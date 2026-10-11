# Demonstration walkthrough (New York road network)

A 10-minute live demo. The point is to **measure on the presenter's machine, in front of the
audience**, and to explain the result honestly, including when the optimized queue does *not*
win. Do not quote a speedup you have not just measured: it depends on the CPU, the graph and
the load on the machine.

## Before the demo (once, ~10 min; needs the network)

```bash
bash scripts/setup/doctor.sh                       # PowerShell: scripts\setup\doctor.ps1
bash scripts/setup/setup.sh                        # builds the engine, installs deps, prepares demo_road
.venv/bin/python scripts/data/prepare_datasets.py dimacs_ny   # 264,346 nodes, ~6 MB, digest-verified
```

(Windows: `.venv\Scripts\python.exe`.) Close other heavy programs; the benchmark pins one core
but a busy machine still adds noise.

## The demo

1. **Start the app** – `bash scripts/setup/run.sh` (PowerShell: `scripts\setup\run.ps1`), open
   <http://127.0.0.1:3000>. The landing page is *Quick Compare*.
2. **Pick `dimacs_ny`**, press *Random pair*, then *Run comparison*. Show:
   * every lane returns the **same distance** (they are all exact; the queue only changes speed);
   * the **ranking comes from the engine's own timings** for this route on this machine, over
     several shuffled rounds. A single route is short, so rounds can disagree; the verdict is
     the majority of rounds, not one number;
   * the **Optimized** lane names the queue it *actually resolved to*. If it says
     `radix_heap`, Optimized fell back and is simply the radix heap: say so, do not present it
     as a win.
3. **A\*** – switch to A\*. On New York, Optimized resolves to the radix heap under A\* (measured
   below: a tie, not a win). This is the honest counter-example.
4. **Benchmarks page → Stored benchmark results** – run the reproduction (below) first, then
   show that the file written under `results/raw/` appears and opens in the Research view with
   a paired comparison and its confidence interval.
5. **Route Explorer** (`/explore?dataset=dimacs_ny`) – click two points, run Dijkstra with the
   binary heap and with LPQ, step the replay. This shows what the queue does, not its speed.

## Measure live (do this on stage, or just before)

```bash
.venv/bin/python scripts/data/prepare_datasets.py dimacs_ny      # if not done
build/release/bin/lpq_benchmark --dataset datasets/processed/dimacs_ny.lpqgraph \
  --queries-file experiments/manifests/dimacs_ny_queries_100.yaml --algorithm dijkstra \
  --repetitions 7 --warmup 3 --cpu-core 2 --seed 20260923 --interleave \
  --experiment-id demo-dimacs_ny-dijkstra --output results/raw/local/demo-ny-dijkstra.json \
  --compare "radix_heap;binary_heap;lpq_optimized"
.venv/bin/python scripts/bench/summarize_paired.py --ref radix_heap results/raw/local/demo-ny-dijkstra.json
```

(Windows: add `.exe`; `scripts\demo\reproduce_benchmark.ps1 -Dataset dimacs_ny` runs a similar
comparison.) The first `lpq_optimized` run on a machine **calibrates** (about 1.7 s measured
here, reported separately and never inside a query's timing) and caches the decision per
graph/CPU/software version; later runs reuse it.

## What was measured while preparing this document (a reference, not a promise)

Cloud VM, Intel Xeon 2.1 GHz, 1-CPU quota (`nproc` = 1), no hardware counters, other work on
the host: **noisy, development-stage evidence**. DIMACS New York, 100 queries, 7 repetitions,
3 warm-up, interleaved, pinned; ratio = time relative to the radix heap (<1 is faster), 95 %
bootstrap interval of the per-query geometric mean:

| Run | Algorithm | `lpq_optimized` vs radix | 95 % CI | faster on | binary heap vs radix |
|---|---|---:|---|---:|---:|
| 1 (cold calibration) | Dijkstra | 0.859 | 0.835–0.886 | 85 % | 1.375 |
| 2 (cached) | Dijkstra | 0.866 | 0.840–0.894 | 81 % | 1.380 |
| 3 (cached) | Dijkstra | 0.867 | 0.840–0.897 | 85 % | 1.346 |
| 4 | A\* (euclidean) | 1.014 | 0.992–1.037 | 47 % | 1.280 |

All 900 Dijkstra and 300 A\* records matched the trusted reference distance. `lpq_optimized`
resolved to `pooled_bucket(width=1, ring=65536)` for Dijkstra and to the radix heap for A\*.
The PR description reported 0.916 for Dijkstra on its own host; this host gave about 0.86 –
the direction reproduces, the size does not transfer between machines, which is why the demo
measures live.

## Say plainly

* The gain comes from queue engineering and a measured per-graph choice, **not** from a
  learned model. The trained queue-selection policy never beat radix on a held-out topology
  (see `docs/experiments/v2-results.md`).
* Online learning is not active: `OnlineLearner::maybe_update` has no call site outside tests,
  so no model weight changes in any run.
* It does not beat radix everywhere: A\* resolved to radix in the run above; Facebook and DIMACS COL are reported to as well in `docs/experiments/v2-results.md` (not re-measured here).
