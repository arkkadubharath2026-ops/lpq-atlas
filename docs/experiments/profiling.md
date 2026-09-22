# Hardware profiling: what this machine can and cannot measure

PRD.md §20 and ESD.md §20 ask for CPU cycles, instructions, branch instructions,
branch misses, cache references, cache misses, LLC misses and memory bandwidth,
collected with Linux `perf` "or equivalent tooling", and require that
**unavailable counters be clearly documented**. This file is that documentation.

Everything below was measured on the development host, not inferred.

## Summary

| Metric class | Available here? | Mechanism |
|---|---|---|
| Wall-clock runtime | **Yes** | `std::chrono::steady_clock`, monotonic |
| Priority-queue operation counts | **Yes** | in-process counters (ESD.md §19) |
| ML feature / inference / verification latency | **Yes** | in-process scoped timers |
| Peak RSS | **Yes** | OS-reported, per process |
| Cycles, instructions, branch misses, cache misses, LLC misses | **No** — see below | would need `perf` with a `cpu` PMU |
| Memory bandwidth | **No** | same root cause |

## Why hardware counters are unavailable

The development host runs Windows 11 with WSL2, and Docker Desktop runs on the
same WSL2 kernel. **The WSL2 kernel does not expose a CPU PMU to the guest.**

Measured inside the container:

```
$ ls /sys/bus/event_source/devices/
breakpoint  kprobe  msr  power  software  tracepoint  uprobe
```

There is no `cpu` entry. Consequently `perf` can count software events
(task-clock, context-switches, page-faults) but reports hardware events as
unsupported, even with `--privileged`:

```
$ docker run --rm --privileged lpq-dev:latest \
    perf stat -e cycles,instructions,branch-misses,cache-misses true

   <not supported>      cycles
   <not supported>      instructions
   <not supported>      branch-misses
   <not supported>      cache-misses
```

This is a hypervisor limitation, not a permissions or configuration problem.
`perf_event_paranoid` is 2 and raising it changes nothing, because the counters
are absent rather than restricted. `linux-perf` is installed in the image
regardless, so the same image collects real counters when run on a native Linux
host.

### Options for obtaining the counters

1. **Native Linux on this hardware** (dual boot, or a separate machine). The
   `scripts/profile/` tooling works unchanged.
2. **Intel VTune Profiler on Windows** — the host CPU is an Intel Core
   i5-1235U, and VTune reads the PMU natively on Windows. This is the
   "equivalent tooling" PRD.md §20 permits, and is the recommended path if the
   experiments must run on this machine.
3. **Report the gap.** Wall-clock, operation counts and all ML-latency metrics
   remain fully available, so every PRD.md §20 metric except the hardware
   counters can still be collected. Analyses that depend on cache or branch
   behaviour must be marked as not measured rather than estimated.

Under no circumstances may a cache-miss or branch-miss figure be reported from
this host: there is no source for one.

## A second, subtler hazard on this host: hybrid cores

The host CPU is a **12th-generation Intel Core i5-1235U**: 10 physical cores in
a hybrid arrangement — 2 performance (P) cores and 8 efficiency (E) cores — and
12 logical threads, with a 1.3 GHz base clock.

This matters for benchmarking more than the missing counters do. The Windows
and Linux schedulers migrate threads between P and E cores freely, and the two
core types differ substantially in IPC and clock. A benchmark thread moved from
a P core to an E core mid-run shows a large slowdown that has nothing to do with
the algorithm under test. On a laptop part, thermal and power limits make such
migrations likely precisely during long benchmark runs.

Consequences for the experimental protocol (PRD.md §18 requires the same
hardware and thread policy for every algorithm):

- CPU affinity must be pinned, and pinned to the **same core type** for every
  algorithm in a comparison. ESD.md §20 already requires affinity to be
  configurable; on this host it is mandatory, not optional.
- Every result record stores the affinity mask actually used.
- Repetition counts must be high enough, and variance reported, so that a
  migration artefact shows up as dispersion rather than hiding in a mean.
- Runs inside Docker additionally record any cgroup CPU quota, so a throttled
  container is never mistaken for a slow algorithm.

## Memory ceiling

The host has 15.7 GB of RAM, of which the WSL2 VM was allocated 8.0 GB at the
time of writing. This bounds the "largest feasible graph" of ESD.md §33 Tier 4.
Graph memory is reported by `CsrGraph::memory_bytes()`, and the dataset pipeline
refuses to load a graph that would exceed the configured budget rather than
letting the run die to the OOM killer midway through a measurement.

## Reproducing these findings

```bash
scripts/profile/check_counters.sh     # prints PMU availability and exits non-zero if absent
```
