# Reproducing the results

Four scripts, in order. Each one stops at the first thing it cannot verify.

```bash
scripts/reproduce/build_all.sh            # build, check every tool exists, run the tests
scripts/reproduce/run_main_experiments.sh # datasets -> training sets -> models -> measurements
scripts/reproduce/generate_tables.sh      # results/raw -> results/tables
scripts/reproduce/generate_figures.sh     # results/raw -> results/figures
```

## What will reproduce exactly, and what will not

**Comparison counts reproduce to the unit, on any machine.** They are
deterministic properties of the algorithm and the query set. `run_main_experiments.sh`
checks them against the values recorded in `docs/experiments/ablation-waterfall.md`
and **fails if they differ**, because a mismatch means the algorithm or the query
set changed and the recorded findings no longer describe the code.

**Timings will not match and are not expected to.** The machine differs. What
should reproduce is the *direction* and rough scale of each effect, not the
nanoseconds. Two figures in particular travel badly:

- the absolute medians, which depend on the CPU;
- the *magnitude* of the `lpq` versus `lpq_model` difference, which was measured
  to vary by 45% with matrix position alone on one machine.

**Nothing is downloaded.** Dataset acquisition is a separate step with its own
checksum verification:

```bash
python scripts/data/prepare_datasets.py dimacs_ny snap_facebook
```

Folding it into the reproduction would mean either silently re-downloading
several gigabytes or silently proceeding with whatever happens to be on disk.
The scripts check the datasets are present and that each query manifest matches
the graph it names, and stop if not.

## What each script refuses to do

ESD.md section 36 asks these scripts to fail loudly on a missing dataset, a
wrong checksum, an incomplete result, or an absent metric. All four failure
modes are silent by nature — none of them crashes, and each produces output that
looks correct — so each is checked explicitly:

| Failure | Where it is caught | What it would otherwise do |
|---|---|---|
| Dataset missing | `run_main_experiments.sh` | measure fewer graphs than the text claims |
| Query manifest from a different graph | `run_main_experiments.sh`, via the manifest checksum the runner enforces | measure a different query set under the right name |
| Tool not built | `build_all.sh`, by name | surface three scripts later as "command not found" |
| Tests failing | `build_all.sh` | reproduce numbers from code that does not pass its own tests |
| Incomplete results | `generate_tables.sh`, `generate_figures.sh` | build a table from a partial matrix, indistinguishable from a whole one |
| Comparison counts changed | `run_main_experiments.sh` | silently invalidate every recorded finding |
| A figure not written | `generate_figures.sh` | leave the figure directory quietly short |

Two of these are verified rather than asserted: the incomplete-results guard was
tested by removing one configuration from a results file and confirming the
figure script refuses it, and the build-failure gate in the Linux matrix was
tested against a stub compiler that exits non-zero.

## Seeds

Every seed is explicit and recorded in the scripts, not defaulted in a library:

| Seed | Used for |
|---|---|
| 42 | query generation (carried in each query manifest) |
| 20260923 | model training, including the train/test node split |
| 20260923 | the paired permutation tests |

The trained model is **rebuilt** from the training set and the training seed
rather than reused from `models/exported/`, which is git-ignored. A reproduction
that depended on an unversioned artifact would reproduce whatever happened to be
lying in that directory.

## Environment

| | |
|---|---|
| Build | CMake + Ninja, Release |
| Compilers verified | GCC 12 and Clang 16 on Linux (`-Werror`), GCC 16 on Windows |
| Sanitizers | Clang 16 Debug with ASan + UBSan + LSan |
| Python | `pip install -r python/requirements.txt` (pinned) |

The Linux matrix runs in a container:

```bash
scripts/build/linux_checks.sh
```

## The one thing that cannot be reproduced here

**No hardware performance counters.** The development host runs under WSL2,
which exposes no CPU PMU, so cycles, cache misses and branch misses are *not
measured* anywhere in this project and no figure is inferred to fill that gap.
`docs/experiments/profiling.md` records that, with the genuine alternatives
(native Linux, or Intel VTune on Windows). A reproduction on a host with
counters would be able to answer questions this one cannot — in particular
whether the bucket substrate's memory behaviour explains the per-extraction
cost, which remains a hypothesis.
