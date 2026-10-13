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

**The order check is part of the pipeline.** `run_main_experiments.sh` runs the ablation ladder twice
on this host: `configs/experiments/dimacs_ny_ablation_v1.yaml` (`lpq_model` last) and
`configs/experiments/dimacs_ny_order_check_v1.yaml` (the same ladder, `lpq_model` first), writing
`results/raw/dimacs_ny_ablation_v1.json` and `results/raw/dimacs_ny_order_check_v1.json`. The second is a
**new measurement**, not a recovery of the historical reversed-order run, whose output file was never
committed and cannot be reconstructed. Comparison counts do not depend on matrix order, so both files are
checked against the same recorded counts. `generate_figures.sh` draws the two paired scatters from them and
**fails** if either results file is missing or incomplete; it no longer exits 0 with a warning.

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
| Order-check results missing | `generate_figures.sh` | draw only the matrix order known to inflate the effect and exit 0 |
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
| 917 | training-query generation (the separate training manifests) |
| 20260923 | model training, including the train/test node split |
| 20260923 | the paired permutation tests |

The trained model is **rebuilt** from the training set and the training seed
rather than reused from `models/exported/`, which is git-ignored. A reproduction
that depended on an unversioned artifact would reproduce whatever happened to be
lying in that directory.

**Training query sets are source-disjoint from the benchmark** (protocol C in
`docs/experiments/static-scorer-training.md`). `run_main_experiments.sh` picks each
dataset's set with `select_training_manifest` (`scripts/reproduce/common.sh`) and
stops if it shares a source node with a benchmark manifest it reports against: the
collector labels from a full single-source search per source, so a shared source is
a shared search. `snap_facebook` trains on
`snap_facebook_training_queries_source_disjoint.yaml` (generated by
`scripts/data/generate_training_queries.py`); `dimacs_ny` on
`dimacs_ny_training_queries.yaml`, which was checked against the 100-query and
30-query benchmark manifests. The earlier pre-correction and pair-disjoint
protocols are kept as labelled historical evidence, not reproduced by this script.
The training manifest's name and SHA-256 are recorded in each model's `.report.json`, and the Atlas model page
reads them (it no longer guesses the manifest from the dataset name). The split guard
(`python/lpq_tools/training/leakage.py`) holds a manifest that declares `source_disjoint` selection to source-level
separation; any other manifest keeps the legacy pair-level verdict, with its shared sources reported.

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

### Verified from a clean clone

`scripts/reproduce/build_all.sh` has been run against a fresh `git clone` of
this repository: 126 targets built and **831/831 tests passed**. That run is
what found the four defects below, none of which is visible from a working tree
that already builds.

**Two prerequisites the script now checks for you.** Both were discovered the
hard way.

- **A C++20 compiler must be the one CMake picks.** The clean-clone run selected
  `C:\MinGW\bin\g++`, which is GCC 6.3.0 from 2016 and first on `PATH`, while
  the compiler this project builds with sat further down it. CMake reported that
  failure from inside a `FindThreads` `try_compile`, twenty lines deep through
  `FetchContent`, naming neither the compiler it had chosen nor the requirement
  it failed. `build_all.sh` now probes for C++20 before configuring and, on
  failure, prints the compiler's path, its version, and what to set:

  ```bash
  CXX=/path/to/modern/g++ CC=/path/to/modern/gcc scripts/reproduce/build_all.sh
  ```

- **Ninja is pinned rather than left to CMake's default.** On Windows outside a
  Visual Studio developer shell that default is NMake Makefiles, and the
  configure fails with `CMAKE_CXX_COMPILER not set`.

**One platform limit that is not fixed, because it cannot be.** On Windows the
build fails under a deep source path — googletest's populate stamp exceeds the
260-character limit — with `Filename longer than 260 characters`, and CMake also
warns that object paths approach `CMAKE_OBJECT_PATH_MAX`. Clone to a short path
such as `C:\lpq`. Enabling long-path support in Windows is the other option and
was not tested here.

## The one thing that cannot be reproduced here

**No hardware performance counters.** The development host runs under WSL2,
which exposes no CPU PMU, so cycles, cache misses and branch misses are *not
measured* anywhere in this project and no figure is inferred to fill that gap.
`docs/experiments/profiling.md` records that, with the genuine alternatives
(native Linux, or Intel VTune on Windows). A reproduction on a host with
counters would be able to answer questions this one cannot — in particular
whether the bucket substrate's memory behaviour explains the per-extraction
cost, which remains a hypothesis.
