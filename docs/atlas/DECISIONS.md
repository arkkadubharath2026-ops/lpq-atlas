# LPQ Atlas decisions

Short records of the choices that shaped the implementation, with the alternatives
considered.

## 1. Observe the search through a compiled-out template seam

**Decision.** `dijkstra()`/`astar()` gain an `Observer` template parameter defaulting to
`NullSearchObserver`; every hook sits behind `if constexpr`. The LPQ is observed through
its existing `ExtractionObserver`.

**Alternatives.** A wrapper queue (cannot see stale-pop filtering or examined arcs without
re-implementing the search's logic); a separate traced Dijkstra (a second implementation
the spec forbids); runtime flags (a branch in the benchmarked hot path).

**Consequence.** One implementation serves the benchmark and the visualiser; the traced
run is checked against the untraced one on every execution.

## 2. FastAPI + filesystem jobs, no database or broker

**Decision.** A single-user research deployment: jobs are directories with `status.json`,
run by bounded thread pools; results are the engine's own files.

**Alternatives.** PostgreSQL/Redis/Celery — rejected by spec §24 until multi-user
requirements exist.

**Consequence.** Restart-safe status (in-flight jobs are marked FAILED on reload); no
migration story needed; every artifact is inspectable on disk.

## 3. OSM enters through DIMACS and the existing converter

**Decision.** `osm.py` extracts the road graph with pyosmium and writes DIMACS
`.gr`/`.co`; `lpq_convert` produces the `.lpqgraph`.

**Alternatives.** A new OSM reader in C++ (a second parser and a second checksum path);
live Overpass queries (not reproducible).

**Consequence.** OSM graphs get the same validation, checksum, metadata and manifest
steps as DIMACS/SNAP, from pinned, dated snapshots.

## 4. Coordinates and arrays reach the web layer through `lpq_export`

**Decision.** The API never parses `.lpqgraph`; the C++ reader exports flat arrays that
numpy maps, cached per checksum.

**Alternative.** A Python reader of the binary format — the drift that
`binary_header.py`'s docstring warns about, and which defect 5 in DISCREPANCIES.md shows
is real.

## 5. Replay synchronised by settled nodes

**Decision.** Compare mode advances both replays by the same number of `NODE_SETTLED`
events.

**Alternative.** Wall clock — explicitly rejected by spec §13.1: it would compare an
instrumented animation's speed, not the algorithms.

## 6. Overflow absorption in the bucket substrate

**Decision.** Maintain "overflow keys ≥ window end" by re-filing covered entries whenever
the base advances (DISCREPANCIES.md, defect 1).

**Alternatives.** A sorted overflow structure (more code on a path the derived width never
reaches); refusing widths below max-weight/1024 (would hide the defect rather than fix
it, and the approximate-mode experiments deliberately vary the width).
