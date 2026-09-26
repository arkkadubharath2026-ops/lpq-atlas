# LPQ Atlas: Instruction Manual

LPQ Atlas is a website for exploring shortest-path search. You pick two points on a map,
choose how the search should work, and the real C++ engine finds the shortest route. You can
then **watch the search happen step by step**, compare two methods side by side, and run proper
benchmarks.

Every number you see comes from the engine. Nothing is simulated or made up.

You do **not** need an account or a password.

---

## Contents

1. [Start the platform](#1-start-the-platform)
2. [Your first route in five minutes](#2-your-first-route-in-five-minutes)
3. [The pages, one by one](#3-the-pages-one-by-one)
4. [Choosing settings](#4-choosing-settings)
5. [Reading the results](#5-reading-the-results)
6. [Problems and fixes](#6-problems-and-fixes)
7. [Stopping the platform](#7-stopping-the-platform)
8. [Good to know](#8-good-to-know)

---

## 1. Start the platform

There are two ways. **Option A (Docker) is the simplest.** Use Option B only if you do not
have Docker or you want to change the code.

### Option A: with Docker (recommended)

You need [Docker Desktop](https://www.docker.com/products/docker-desktop/) installed and running.

1. Open a terminal in this folder (the one that contains this manual).
2. Run:

   ```bash
   docker compose up --build
   ```

   The first time takes several minutes, because it compiles the C++ engine and builds the
   website. Later starts take a few seconds.
3. When the terminal shows lines like `Ready` / `Uvicorn running`, open
   **<http://localhost:3000>** in your browser.

If ports 3000 or 8000 are already in use on your computer, pick others:

```bash
ATLAS_WEB_PORT=3001 ATLAS_API_PORT=8001 docker compose up --build
```

and open <http://localhost:3001> instead. (In Windows PowerShell, set them first with
`$env:ATLAS_WEB_PORT="3001"; $env:ATLAS_API_PORT="8001"`.)

The Docker setup uses the datasets, models and stored results already in this folder. If a
dataset is not prepared yet, you can prepare it from the **Datasets** page (see
[section 3.6](#36-datasets)).

### Option B: without Docker

You need:

| Tool | Version | Check with |
|---|---|---|
| C++ compiler (GCC or Clang) | GCC 12+ or Clang 16+ | `g++ --version` |
| CMake and Ninja | CMake 3.24+ | `cmake --version`, `ninja --version` |
| Python | 3.13 | `python --version` |
| Node.js | 22 | `node --version` |

> **Windows users:** if `g++ --version` prints an old version (for example 6.3 from
> `C:\MinGW`), put a modern GCC first on your `PATH`, e.g. WinLibs GCC.

Run these commands once, from this folder:

```bash
# 1. Build the engine (a few minutes)
cmake --preset release
cmake --build build/release

# 2. Install the Python packages
pip install -r python/requirements-dev.txt -r atlas/api/requirements.txt

# 3. Download and prepare the small Monaco map (under 1 MB)
python scripts/data/prepare_datasets.py osm_monaco

# 4. Install and build the website
cd atlas/web
npm ci
npm run build
cd ../..
```

Then, **every time** you want to use the platform, open **two terminals** in this folder:

```bash
# Terminal 1: the API (the part that talks to the engine)
python -m uvicorn atlas_api.app:app --app-dir atlas/api --port 8000
```

```bash
# Terminal 2: the website
cd atlas/web
npx next start -p 3000
```

Open **<http://localhost:3000>**.

### How to tell it is working

* <http://localhost:3000> shows the LPQ Atlas home page with a table of stored experiments.
* <http://localhost:8000/api/health> shows a short JSON message.
* <http://localhost:8000/docs> lists every API endpoint, if you want to use the API directly.

---

## 2. Your first route in five minutes

1. Open <http://localhost:3000> and click **Open the Route Explorer**.
2. At the top left, **Dataset** should say `osm_monaco`. A map of Monaco appears, with the
   road network drawn on it.
3. **Click the map once** to set the **source** (start point). A marker appears.
4. **Click again** somewhere else to set the **destination**.
   Your clicks "snap" to the nearest road junction; the page tells you how far it moved.
5. Leave the settings as they are (Dijkstra, Binary heap, exact) and press **Run**.
6. After a moment:
   * the **right panel** shows the distance, the number of steps, and the timing;
   * the **map** shows the route that was found.
7. Press **▶ Play** under the map to **watch the search spread out** from the source until it
   reaches the destination. Change **speed** to make it faster or slower.
8. Now change **Priority queue** to **LPQ (control scorer)** and press **Run** again. The
   answer is the same distance; the counts and timing tell you how the learned queue did.

Want to start over? Click the map again: the next click becomes a new source.

---

## 3. The pages, one by one

The menu at the top of every page takes you to each part.

### 3.1 Route Explorer

Find one route and watch how the search found it.

**Left panel: set up the search**

| Control | What it does |
|---|---|
| **Dataset** | Which map or graph to search. Greyed-out entries are not prepared yet. |
| **Source node / Destination node** | Filled in when you click the map. You can also type a node number. |
| **Random pair (seeded)** | Appears for graphs with no map. Picks two nodes for you. |
| **Algorithm, Heuristic, Priority queue, Correctness policy** | How the search works; see [section 4](#4-choosing-settings). |
| **Trace** | How much detail to record for the animation (see below). |
| **Timed repetitions** | How many extra times to run the search just to measure speed (default 5). `0` means "don't time it". |
| **Run** | Starts the search. |
| **Cancel** | Stops a search that is still running. |
| **Compare…** | Opens the Compare page with the same two points already filled in. |

**Trace levels**

| Level | Use it when |
|---|---|
| `visual` (default) | You want to watch the search on the map. |
| `full` | You want every single queue operation in the event list. Slower on big graphs. |
| `summary` | You only need the summary events, not the step-by-step detail. |
| `none` | You only want the numbers. Fastest. |

**Middle: the map and the replay**

* Colours: **blue or purple** = places the search has finished with (purple means the learned
  queue was used); **light blue** = places it has reached but not finished; **teal line** = the
  route found; **teal dot** = source; **red dot** = destination.
* **Basemap on/off** (top right of the map) hides the street map so you see only the graph.
* Playback buttons:
  * **▶ Play / ❚❚ Pause** plays or pauses the replay.
  * **Step event** moves forward one recorded event.
  * **Step node** moves forward to the next finished node.
  * **Restart** and **End** jump to the beginning or the end.
  * The **slider** jumps to any point in the replay.
  * **speed** sets how many events play per second.

**Right panel: the results.** See [section 5](#5-reading-the-results).

> **Big graphs.** On large graphs (the DIMACS road networks) a long search can produce more
> steps than the recorder keeps (400,000 events), so the animation shows only the start of it
> and the page says **truncated**. The distance and all numbers are still
> complete. Graphs without coordinates (for example `snap_facebook`) show numbers but no map.

### 3.2 Compare

Run two configurations, **A** and **B**, on the **same** query and see the difference.

1. Choose a dataset and click the map for source and destination (or arrive here from
   **Compare…** on the Route Explorer).
2. Set up column **A** and column **B**, for example *Binary heap* against *LPQ (control scorer)*.
3. Press **Run both**.
4. The two searches replay **side by side**. **Step (1 settled node each)** moves both forward
   by the same amount of work, so you compare work done, not animation speed.
5. **Measured differences** lists how the numbers differ.

The page never declares a "winner". It shows the measurements and lets you judge.

### 3.3 Benchmarks

Measure speed properly over many queries, not just one.

1. **Dataset & queries:** pick a dataset. For **Query set**, pick a saved set or leave it as
   **generate from seed**. Set **Queries** (how many start/end pairs), **Seed**, **Repetitions**
   and **Warm-up**.
2. **Algorithm & policy:** Dijkstra or A*, exact or approximate.
3. **Queues:** tick every queue you want to include. Choose the **Baseline** to compare
   the others against.
4. Press **Run benchmark**. Larger benchmarks can take minutes. **Cancel** stops one.
5. The **Results** show medians, spread, and paired statistics against the baseline.
   **Export raw records (JSON)** downloads every measurement.
6. **History** lists earlier benchmarks; click one to open it again.

Every query in a benchmark is checked against a trusted reference search. A wrong answer
stops the benchmark instead of producing a misleading time.

### 3.4 Research

Browse the project's **stored experiment results** (the ones behind the report).

* Pick an experiment from **Stored experiments** to see its charts and statistics.
* **Research notes** shows the written findings.

### 3.5 Models

The trained models the learned queue can use. For each one:

* which dataset it was trained on;
* whether its training and test queries are kept apart (**DISJOINT** is good);
* whether the C++ engine can load it.

Click a model name to see its details. On that page, the **Node inspector** shows the model's
inputs and score for any nodes you type (for example `0,1,2`) after you press **Score**.

### 3.6 Datasets

Every map and graph the platform knows about, with its size, source and status.

* **READY** means you can use it.
* If a dataset is not ready, open it and press **Prepare dataset**. The platform downloads the
  pinned file, checks its fingerprint (checksum), and converts it. Small maps take seconds;
  the large DIMACS/SNAP graphs take longer and need more disk space.
* From a dataset's page you can jump straight to **Open in Route Explorer** or **Benchmark it**.

| Dataset | What it is | Size | Map? |
|---|---|---|---|
| `osm_monaco` | Monaco roads (OpenStreetMap) | 5,692 nodes | yes, **best for learning** |
| `osm_liechtenstein` | Liechtenstein roads (OpenStreetMap) | 29,945 nodes | yes |
| `dimacs_ny`, `dimacs_col`, `dimacs_fla` | New York, Colorado, Florida road networks | 264k – 1.07M nodes | yes (long animations are truncated) |
| `synth_ratio1_ny` | New York roads with synthetic weights | 264k nodes | yes (long animations are truncated) |
| `snap_facebook` | A social network | 4,039 nodes | no, numbers only |
| `snap_roadnet_pa`, `snap_roadnet_ca` | Pennsylvania, California road networks | 1.1M – 2M nodes | no, numbers only |

### 3.7 Architecture / Learn

A plain explanation of how the platform works and what each part of the learned queue does.
Read this if the terms on other pages are unfamiliar.

### 3.8 A single run's page

Every run gets its own page at `/runs/<run id>` (the link appears under the **Run** button).

* **Open in Route Explorer** reopens the run on the map.
* **Download result JSON** saves every number from the engine.
* **Download trace JSONL** saves every recorded step.

Copy the page address to come back to the same run later.

---

## 4. Choosing settings

| Setting | Options | Simple advice |
|---|---|---|
| **Algorithm** | **Dijkstra**, **A\*** | Dijkstra searches in every direction. A* uses the straight-line distance to aim at the destination, so it usually does less work. |
| **Heuristic** (A* only) | auto, haversine, euclidean, zero | Leave it on **auto**. `zero` makes A* behave like Dijkstra. |
| **Priority queue** | **Classical:** Binary heap, 4-ary heap, Pairing heap, Fibonacci heap, Radix heap | Standard data structures. **Binary heap** is the usual baseline; **Radix heap** is often the fastest. |
| | **Learned:** LPQ (control scorer), LPQ + static model, LPQ + adaptive controller | The learned priority queue this project studies (see below). |
| **Correctness policy** (learned queues only) | **exact**, **approximate** | **exact** always returns the true shortest distance. **approximate** may return a slightly longer route, at most (1 + ε) times the shortest; ε = 0.1 means "at most 10% longer". |

**The three learned queues**

* **LPQ (control scorer):** the learned queue's machinery with no model. Useful to see what the
  machinery itself costs.
* **LPQ + static model:** a trained model gives every node a score before the search starts.
  Only available where a model was trained for that dataset (`dimacs_ny`, `snap_facebook`);
  elsewhere it is greyed out with "no model for this dataset".
* **LPQ + adaptive controller:** the queue adjusts its own policy during the search from what
  it observes. Tick **Also update the policy model online** to let it learn as it goes.

---

## 5. Reading the results

The right panel of the Route Explorer (and each column in Compare) shows:

| Item | Meaning |
|---|---|
| **Distance** | Length of the route found. On OpenStreetMap graphs it is shown in decimetres **and** kilometres. It is the *shortest* route by distance, not the *fastest* by travel time. |
| **Reference (exact)** | The true shortest distance, from a separate trusted search. |
| **Path** | Number of road segments (hops) in the route. |
| **Path cost verified** | "yes" means the route's segments were re-added and really do sum to the distance. |
| **exact: matches reference** (green) | Exact mode found the true shortest distance. |
| **ratio … ≤ … holds** (green) | Approximate mode stayed within its promised limit. |
| **MISMATCH** / **VIOLATED** (red) | Something went wrong. This should never happen; please report it. |
| **Destination unreachable** | No road connects the two points in that direction. This is a correct answer, not an error. |
| **Benchmark timing** | Speed of the search, measured on separate runs **with recording turned off**. The median is the headline number. A **noisy** flag means the timings varied a lot. |
| **Operations** | How much work the search did: nodes settled, edges relaxed, queue operations. |
| **Learned queue / LPQ decisions** | For learned queues only: how often a candidate was accepted, rejected, or the queue fell back to the safe method. |
| **Hardware counters** | Shown as *unavailable* on this computer. They are never estimated. |
| **Provenance** | Exactly which engine version, dataset and settings produced these numbers. |
| **trace reproduces the measured run** | The recorded animation matches the measured search exactly. |
| **truncated: N events not recorded** | The animation shows only the start of the search. The numbers are still complete. |

**Why is the animation not the speed?** Recording every step slows the search down, so the
platform times the search separately with recording turned off. The replay speed you choose is
for watching only; it says nothing about how fast the algorithm is.

---

## 6. Problems and fixes

| Problem | Fix |
|---|---|
| A red box says **"The API is unavailable."** | The API is not running. Start it (Terminal 1 in Option B), or check `docker compose` is still running. |
| The website does not open at localhost:3000. | Check that the website terminal (or `docker compose`) is still running. If another program uses port 3000, use other ports (see [Option A](#option-a-with-docker-recommended)). |
| A dataset is greyed out in the list. | It is not prepared. Go to **Datasets**, open it, press **Prepare dataset**. |
| **LPQ + static model** is greyed out. | No model was trained for that dataset. Use `dimacs_ny` or `snap_facebook`, or another queue. |
| No map, only numbers. | The graph has no coordinates (SNAP graphs) or is too large to draw. This is expected. |
| **Run** stays disabled. | You need both a source and a destination, and for LPQ + static model you must choose a model. |
| "Destination unreachable". | Not a bug: some roads are one-way or disconnected. Pick different points. |
| A run says **FAILED**. | Open its run page to see the error message. Very large graphs can hit the time limit (180 s); try `Trace: none` or a smaller graph. |
| `cmake --preset release` fails on Windows with a very old compiler. | Put a modern GCC first on `PATH` (see Option B). |
| A build directory "was created from a different path" error. | Delete or rename `build/release` and run `cmake --preset release` again. |

---

## 7. Stopping the platform

* **Docker:** press `Ctrl+C` in the terminal, then run `docker compose down`.
  Add `-v` (`docker compose down -v`) to also delete the runs you made inside Docker.
* **Without Docker:** press `Ctrl+C` in both terminals.

Your runs and benchmarks are saved to disk and are still there next time:

* in `results/atlas/` when you run without Docker;
* in the `atlas-work` Docker volume when you run with Docker.

---

## 8. Good to know

* **Everything is real.** Distances, counts and timings come straight from the C++ engine.
  Every answer is double-checked against a trusted reference search.
* **Negative results stay visible.** On many queries the learned queue is *slower* than the
  classical heaps. The platform shows this honestly; that is one of the project's findings.
* **Road maps are simplified.** OpenStreetMap roads ignore turn restrictions, and distances are
  straight-line lengths of road segments, so routes are the shortest, not the fastest.
* **Map data** © OpenStreetMap contributors (ODbL). DIMACS road networks are from the
  9th DIMACS Implementation Challenge; SNAP graphs are from the Stanford Network Analysis Project.
* **More detail:**
  * [docs/atlas/README.md](docs/atlas/README.md): technical overview and tests.
  * [docs/atlas/ENGINE_CONTRACT.md](docs/atlas/ENGINE_CONTRACT.md): the engine's inputs and outputs.
  * [docs/atlas/DISCREPANCIES.md](docs/atlas/DISCREPANCIES.md): known issues and fixes.
