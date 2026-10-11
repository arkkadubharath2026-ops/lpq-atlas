# Local setup: build, test, run and benchmark on your own machine

This page takes a fresh `git clone` to a running LPQ Atlas (FastAPI API on port 8000,
Next.js web UI on port 3000) and to a baseline-versus-alternatives latency benchmark, on
Windows or Linux. Nothing here depends on a cloud session, on uncommitted files, on
credentials, or on data that cannot be re-acquired: the demonstration dataset is committed
(0.5 MB) and the real datasets are downloaded from their public hosts by a checksum-verified
pipeline.

## The three commands

Run them from the repository root.

| | Linux / macOS / Git Bash | Windows PowerShell |
|---|---|---|
| 1. Check the machine | `bash scripts/setup/doctor.sh` | `powershell -ExecutionPolicy Bypass -File scripts\setup\doctor.ps1` |
| 2. Set up, test, launch | `bash scripts/setup/run.sh --test` | `powershell -ExecutionPolicy Bypass -File scripts\setup\run.ps1 -Test` |
| 3. Benchmark | `bash scripts/demo/reproduce_benchmark.sh` | `powershell -ExecutionPolicy Bypass -File scripts\demo\reproduce_benchmark.ps1` |

The `.sh` files are invoked with `bash` so they work whether or not git preserved their
executable bit (a repository committed from Windows often does not).

* **`doctor`** reports what is installed and prints the install command for anything
  missing. It changes nothing and exits non-zero if an essential tool is absent.
* **`run --test`** sets the clone up on first use (creates `.venv`, installs the Python
  and npm dependencies, configures and builds the engine with the CMake preset `release`,
  prepares the offline `demo_road` dataset, builds the web UI), runs every test suite, then
  starts both servers and waits until they answer. When it prints the URL, open
  <http://127.0.0.1:3000>. Press Ctrl+C to stop both servers. On later runs the setup steps
  find their work already done and are skipped.
* **`reproduce_benchmark`** runs `lpq_benchmark --interleave` on `demo_road` (and on
  `dimacs_ny` if you have prepared it), prints the comparison table and a summary, and
  writes each raw result to a **new** file under `results/raw/local/`.

`run` without `--test` / `-Test` skips the test suites. `setup` can be run on its own
(`bash scripts/setup/setup.sh`, `scripts\setup\setup.ps1`) to prepare everything without
launching, and `test` on its own (`bash scripts/setup/test.sh [cpp] [python] [api] [web]`).

## Prerequisites

| Tool | Version | Why |
|---|---|---|
| Git | any, **and a real clone** | `lpq_benchmark` refuses to run if the binary was built without git provenance (ESD.md section 40). A downloaded zip has no `.git`. |
| C++20 compiler | GCC >= 11, Clang >= 14, MSVC 2022, or MinGW-w64 GCC >= 12 | the engine uses concepts and `<version>` |
| CMake | >= 3.25 | `CMakePresets.json` uses preset schema version 6; CMake 3.24.3 rejects it with `Unrecognized "version" field` (checked), although `CMakeLists.txt` and the presets file say 3.24 |
| Ninja | any | the presets select the Ninja generator |
| Python | **3.11 to 3.13** (CI uses 3.13) | see the note below |
| Node.js + npm | Node >= 20 (CI uses 22) | the web UI (Next.js 16) |
| Disk | about 1.3 GB | measured on Linux: `.venv` 568 MB, `atlas/web/node_modules` 617 MB, `build/` 45 MB |
| Network | at setup time only | pip, npm and the googletest download at configure time (see "Offline"); none at run time with the demo dataset |

**Python 3.14 is not supported yet.** `python/requirements.txt` pins numpy 2.2.4, pandas
2.3.1 and scipy 1.15.2, which publish no wheels for 3.14; pip would try to compile them
and normally fails. `doctor` detects this and says so. Install Python 3.13 next to it;
the scripts look for 3.13, 3.12 and 3.11 before falling back to whatever `python3` is.

### Linux (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl python3 python3-venv python3-pip
```

* Ubuntu 22.04 ships GCC 11 (enough) but CMake 3.22 (too old; 3.25 or newer is needed): `pip install --user cmake`
  or use <https://apt.kitware.com>.
* `apt`'s `nodejs` is usually too old (Ubuntu 24.04 has 18). Install Node 22:
  `curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt install -y nodejs`
  (or use nvm).
* Other distributions: install the same tools with the package manager; `pip install cmake ninja`
  works anywhere.

### Windows

Clone to a **short path** such as `C:\lpq`: the googletest download creates files whose paths
exceed Windows' 260-character limit when the repository sits deep in your profile
(`docs/reproducing.md` records this failure). `doctor` warns when the path is long.

```powershell
winget install Git.Git Kitware.CMake Ninja-build.Ninja Python.Python.3.13 OpenJS.NodeJS.LTS
git clone <repo-url> C:\lpq
```

Then one C++ toolchain, either:

* **MSYS2 MinGW-w64 GCC (the toolchain this project's Windows builds have used).**
  Install MSYS2 (`winget install MSYS2.MSYS2`), open the "MSYS2 UCRT64" shell and run
  `pacman -S --needed mingw-w64-ucrt-x86_64-gcc` (GCC >= 12). The setup scripts find
  `C:\msys64\ucrt64\bin\g++.exe` on their own, so it need not be on `PATH`. An old MinGW
  (for example `C:\MinGW`, GCC 6.3) earlier on `PATH` is skipped, and `doctor` names it
  as rejected.
* **Visual Studio 2022 Build Tools** (MSVC):
  `winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
  The scripts import the developer environment themselves (via `vswhere` and
  `vcvars64.bat`), so a plain PowerShell window is fine. The CMake files carry MSVC flags,
  but the Windows builds recorded in this repository used GCC; treat MSVC as less proven.

The PowerShell scripts are invoked as `powershell -ExecutionPolicy Bypass -File <script>`
(works in Windows PowerShell 5.1 and PowerShell 7), or run
`Set-ExecutionPolicy -Scope Process Bypass` once in your window and call `.\scripts\setup\run.ps1 -Test`.
Git Bash also works with the `.sh` scripts.

## What you get after setup

| Path | Content |
|---|---|
| `.venv/` | Python environment (`pip install -r python/requirements-dev.txt -r atlas/api/requirements.txt`) |
| `atlas/web/node_modules/`, `atlas/web/.next/` | web dependencies and production build |
| `build/release/bin/` | `lpq_run`, `lpq_benchmark`, `lpq_convert`, `lpq_export`, `lpq_model_info` and the other tools. This is the **only** place the API looks for binaries (`python/lpq_tools/utilities/paths.py`; the executable gets `.exe` on Windows, and the Ninja generator is single-config so there is no `Release/` subfolder). |
| `datasets/processed/demo_road.lpqgraph` | the converted demo graph; its metadata is `datasets/metadata/demo_road.json` and its query manifest `experiments/manifests/demo_road_queries.yaml` (both committed) |
| `results/atlas/launcher/` | server logs |
| `results/raw/local/` | benchmark results (git-ignored) |

### The demonstration dataset

`demo_road` is **synthetic**: a 19,600-node, 55,986-arc road-like network (two-way streets,
three road classes, integer travel times in deciseconds, coordinates in open ocean so no
basemap pretends to match it). It is generated from a fixed seed by
`scripts/demo/make_demo_graph.py`; the two DIMACS-format files it produces are committed
under `datasets/demo/` (`.gz`, 0.5 MB, so no line-ending conversion can touch them), their
SHA-256 digests are pinned in `configs/datasets/demo_road.yaml`, and
`python scripts/demo/make_demo_graph.py --check` verifies them against the generator.
`scripts/demo/prepare_demo.py` copies them into `datasets/raw/`, runs the normal dataset
pipeline (`lpq_convert`, metadata, manifest) and fails if the converted graph's checksum
differs from the committed metadata. No network is involved.

Use it to see the whole system work. Anything you report as a **result** should come from a
real dataset.

### Real datasets (need the network)

```bash
python scripts/data/prepare_datasets.py dimacs_ny        # ~6 MB download, 264,346 nodes
python scripts/data/prepare_datasets.py --list           # everything declared
```

`dimacs_ny` is downloaded from the 9th DIMACS challenge site and verified against the digests
pinned in its config. The OpenStreetMap datasets (`osm_monaco`, `osm_liechtenstein`) come from
`download.geofabrik.de`; that download failed with *connection reset* in the cloud sandbox
this documentation was written in, so treat it as **environment-dependent** and fall back to
`dimacs_ny` or `demo_road` if it fails for you. Note that `prepare_datasets.py` rewrites
`datasets/metadata/index.json` from only the datasets it was asked to prepare, so a git diff of
that file after preparing one dataset is expected.

## Reproducing the latency benchmark

```bash
bash scripts/demo/reproduce_benchmark.sh                         # demo_road (+ dimacs_ny if prepared)
bash scripts/demo/reproduce_benchmark.sh --repetitions 21 --cpu-core 3
bash scripts/demo/reproduce_benchmark.sh --dataset dimacs_ny --compare "binary_heap@baseline;bucket_queue@bucket;lpq_direct:front=0@lpq_direct"
```

(PowerShell: `-Repetitions`, `-Warmup`, `-CpuCore`, `-Compare`, `-Dataset`.)

What it runs, per dataset:

```
lpq_benchmark --algorithm dijkstra --dataset datasets/processed/<id>.lpqgraph \
    --queries-file experiments/manifests/<id>_queries[_100].yaml \
    --repetitions 11 --warmup 3 --interleave --compare "<spec>" \
    --experiment-id local-<UTC time>-<id> --output results/raw/local/<UTC time>-<id>.json
```

* The default `--compare` spec is `binary_heap@baseline;pairing_heap@pairing_heap;bucket_queue@bucket_queue;lpq_direct@lpq_direct`:
  the binary heap is the baseline, the other rows are alternatives. Which is fastest is what
  the run measures; nothing is assumed. The syntax is `name[:w=N,ring=N,front=N][@label];...`.
* `--interleave` measures queries in the outer loop and shuffles the order of the
  configurations for every query (seeded), so slow drift of the host (thermal state, frequency
  scaling, other processes) hits every configuration equally instead of favouring whichever ran
  first.
* Every answer is checked against the trusted reference search; a wrong path aborts the run.
* The fixed query manifests (seed 42) make the query set identical on every machine.
  `demo_road` uses 200 queries; `dimacs_ny` uses the committed 100-query manifest.
* Raw results are immutable: the binary refuses to overwrite a file, so each run writes a new
  timestamped file. Two runs of the same dataset within the same second collide; run again.

### Reading the benchmark output

After each run you get the binary's table and `scripts/demo/summarize_results.py`'s view of the
same file:

| Column / field | Meaning |
|---|---|
| `median ns` | the median over queries of each query's median time (nanoseconds) |
| `q-MAD ns`, `q-spread` | median absolute deviation across queries: workload heterogeneity, not noise |
| `noise` | typical within-query relative MAD: the measurement noise |
| `[NOISY]` | some measurements exceeded the noise threshold. Kept, never dropped. |
| `[UNPINNED]` | the process could not pin itself to a core (see troubleshooting) |
| `answers = reference` | how many answers matched the reference search (must be all) |
| `publication_quality` | per-measurement flag in the JSON; expect `false` on a laptop, a VM or a dirty working tree |

The medians are in the printed table; the full per-repetition samples, operation counters,
environment (CPU model, compiler, flags, git commit and whether the tree was dirty) are in the
JSON. Hardware counters (cycles, cache misses, ...) are reported as *not collected* unless the
host exposes a PMU; WSL2 and most VMs do not, and nothing is estimated in their place
(`docs/experiments/profiling.md`).

### How much to trust a number from this script

**Timings are machine-dependent.** They depend on the CPU, its frequency policy, background
load, the compiler and the OS. The **comparison counts** (`comparisons`, `pops` columns) are
deterministic properties of the algorithm and the query set and are the part that should agree
between machines. **Cloud and virtual-machine timings are development-stage evidence**, not
publication results: shared hosts, unpinned or migrating threads, and noisy neighbours move
medians by more than many of the differences being measured. For numbers you intend to
report, use a quiet physical machine, pin to one core (`--cpu-core N`, ideally a performance
core), close other applications, and use more repetitions (`--repetitions 21`). Recorded
results and their methodology are in [docs/experiments/](experiments/README.md); the full
research pipeline (trained models, tables, figures) is `docs/reproducing.md`.

## Environment variables

| Variable | Effect |
|---|---|
| `ATLAS_API_PORT` (8000), `ATLAS_WEB_PORT` (3000) | ports for `run`. The production web build bakes in the API address, so changing the API port rebuilds the UI once. |
| `CXX`, `CC` | use this compiler exclusively (otherwise auto-detected) |
| `LPQ_SETUP_PYTHON` | interpreter to create `.venv` with |
| `LPQ_JOBS` | build parallelism (`cmake --build -j`) |
| `LPQ_USE_SYSTEM_GTEST=1` | use an installed GoogleTest instead of downloading it |
| `ATLAS_MAX_JOBS`, `ATLAS_RUN_TIMEOUT_S`, `ATLAS_WORK_DIR`, ... | API limits, see `docs/atlas/README.md` |

Options: `setup --no-tests` (do not build the C++ tests; no googletest download),
`setup --skip-web`, `setup --force`, `run --dev` (serve the UI with `next dev`: no production
build, slower page loads).

## Offline use

With the demo dataset nothing needs the network **at run time**. At setup time pip, npm and,
for the C++ tests, the googletest download (GitHub, at CMake configure time) do. Without
GitHub access either install GoogleTest and set `LPQ_USE_SYSTEM_GTEST=1`, or use
`setup --no-tests` (the C++ tests are then neither built nor run, and `test` reports
that). The map view loads basemap tiles from `tile.openstreetmap.org`; offline the tiles are
blank, the graph still draws.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `doctor`: no usable C++20 compiler, or CMake says the compiler "does not support CXX20" | compiler older than GCC 11 / Clang 14 / MinGW GCC 12, or an old one first on `PATH` (for example `C:\MinGW`, GCC 6.3) | install a newer one (see Prerequisites); `doctor` lists each compiler it rejected and why. Force one with `CXX=/path/to/g++ bash scripts/setup/setup.sh` (PowerShell: `$env:CXX='C:\msys64\ucrt64\bin\g++.exe'`). If `build/release` was configured with another compiler, delete `build/release` first. |
| `doctor`: Python "found 3.14 only" / pip fails building numpy, scipy or pandas | the pinned versions have no 3.14 wheels | install Python 3.13 (3.11 and 3.12 also work); delete `.venv` if it was created with another version |
| `python3 -m venv` fails, "ensurepip is not available" | Debian/Ubuntu split the venv module out | `sudo apt install python3-venv` |
| CMake too old (3.22 on Ubuntu 22.04; 3.24 is also too old) | distribution package | `pip install --user cmake`, or apt.kitware.com, or `winget install Kitware.CMake` |
| Configure fails fetching googletest | no access to github.com (offline, proxy, firewall) | set `HTTPS_PROXY`, or `LPQ_USE_SYSTEM_GTEST=1` with GoogleTest installed, or `setup --no-tests` |
| Windows: build fails, "Filename longer than 260 characters" | deep clone path | clone to `C:\lpq` |
| Windows: "running scripts is disabled on this system" | execution policy | invoke with `powershell -ExecutionPolicy Bypass -File ...` |
| `FAILED: port 8000 is already in use` (or 3000) | another program, or a server left from an earlier run | stop it, or `ATLAS_API_PORT=8001 ATLAS_WEB_PORT=3001 bash scripts/setup/run.sh`. To find the owner: `ss -ltnp \| grep :8000` (Linux), `Get-NetTCPConnection -LocalPort 8000` (PowerShell). |
| `lpq_benchmark`: "refusing to run: this binary carries no git provenance" | built from a source tree without `.git` (zip download) | `git clone` the repository, delete `build/release`, run setup again |
| `lpq_benchmark` warns "built from a dirty working tree" | uncommitted changes (including untracked files) when CMake last configured | informational: the results record `git_dirty: true`. Commit or stash, delete `build/release`, rebuild, for clean provenance. |
| `dataset 'dimacs_ny' is missing` | real datasets are not part of the clone | `python scripts/data/prepare_datasets.py dimacs_ny` |
| `demo_road` missing, or its checksum differs | demo not prepared, or a changed checkout | `python scripts/demo/prepare_demo.py` (add `--force` to reconvert); `python scripts/demo/make_demo_graph.py --check` verifies the committed files |
| `refusing to overwrite an existing results file` | two runs in the same second | run again |
| `[UNPINNED]`, or a warning that CPU affinity could not be set | no permission to set affinity (some Windows configurations, VMs, containers, restricted cgroups) | the run continues unpinned and says so; numbers are noisier. Try `--cpu-core N` for a core you are allowed to use, run on bare metal, or accept the result as development-stage evidence. Do not use `--require-pinning` where pinning is impossible: it aborts. |
| `[NOISY]` on most rows | busy or shared host, thermal throttling, laptop on battery | close other programs, plug in, raise `--repetitions`, pin to a performance core; nothing is dropped from the JSON |
| Web UI loads but shows "API unreachable" | API not running, or the UI was built for a different API port | check `results/atlas/launcher/api.log`; run via `run.sh`, which rebuilds the UI when the port changed |
| `npm ci` fails with an engine/version error | Node older than 20 | install Node 22 |
| Map is blank | no internet for OSM basemap tiles, or the demo graph (open ocean) has no basemap | expected; the graph and routes still draw |
| Two C++ tests fail in `ctest` | not a setup problem: the failing test names identify the module | see the test output; `bash scripts/setup/test.sh` runs every other suite regardless |

## Status of these instructions

Be aware of what has and has not been exercised:

* **Linux (Ubuntu 24.04 container, GCC 13, CMake 3.28, Python 3.13, Node 22):** the Bash
  scripts were run end to end from a fresh `git worktree` of this repository: `setup` (venv,
  `npm ci`, engine build, demo dataset, `next build`), `test`, `run` (API and web answered,
  a route was computed through the API and its distance matched the reference search; Ctrl+C /
  SIGTERM stopped both servers), `reproduce_benchmark` on `demo_road` and `dimacs_ny`, and
  `doctor` with missing and unsupported tools simulated.
* **PowerShell scripts:** parsed with the PowerShell 7.4 parser and run under PowerShell 7.4
  on **Linux** (`doctor`, `setup`, `run`, `reproduce_benchmark`, and `test` for the python and
  api suites; all paths in them use forward slashes, which Windows accepts). On Linux,
  `run.ps1` was stopped with SIGTERM, which does not run its cleanup, so a server could be left
  running; Ctrl+C on Windows is the intended way and is untested.
  They have **not been run on Windows**. Windows-only parts, the MSYS2 and Visual Studio
  compiler discovery, `vcvars64.bat` import, `taskkill`-based shutdown and Windows PowerShell
  5.1 behaviour, are written from documentation and unverified. If one misbehaves, the
  equivalent manual commands are in the next section.
* Timings from the development sandbox (a shared cloud VM) are not quoted anywhere in this
  repository as results.

## Manual equivalent (what the scripts do)

```bash
python3.13 -m venv .venv && .venv/bin/pip install -r python/requirements-dev.txt -r atlas/api/requirements.txt
(cd atlas/web && npm ci)
cmake --preset release && cmake --build build/release        # Windows: set CXX/CC to the compiler first
.venv/bin/python scripts/demo/prepare_demo.py                  # Windows: .venv\Scripts\python.exe
ctest --preset release-fast
(cd python && ../.venv/bin/python -m pytest -q) && (cd atlas/api && ../../.venv/bin/python -m pytest -q)
(cd atlas/web && npm run lint && npm run typecheck && npm test && npm run build)
PYTHONPATH=python .venv/bin/python -m uvicorn atlas_api.app:app --app-dir atlas/api --port 8000 &
(cd atlas/web && npx next start -p 3000)
```
