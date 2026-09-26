# Discrepancy register: resolution status

Spec §5 lists ten discrepancies between the abstract, the documentation and the code.
This file records what LPQ Atlas did about each, and the further defects found while
building it. Nothing below was removed to make the documentation look consistent.

## Spec items

| id | item | status | evidence |
|---|---|---|---|
| D-01 | OSM promised, not implemented | **Resolved** | `python/lpq_tools/data/osm.py` (documented road policy), `configs/datasets/osm_*.yaml` (dated Geofabrik snapshots, SHA-256 pinned), `datasets/metadata/osm_*.osm.json` (provenance), `python/tests/test_osm.py`; Dijkstra, A* (haversine) and LPQ verified exact on `osm_monaco` / `osm_liechtenstein` |
| D-02 | "online ML" broader than the central path | **Resolved in the UI** | every result states `lpq.scorer`, `adaptive_controller`, `online_learning`; the Models page states the static-scorer mode |
| D-03 | rich features vs optimized path | **Exposed** | `lpq.features` lists the six static features used; the Model page shows which weights are non-zero as read by the C++ loader |
| D-04 | model zoo vs selected model | **Resolved** | Models page separates implementations, trained artifacts, the queue that uses them, and split status |
| D-05 | cache misses | **Unavailable, stated** | `hardware_counters.available = false` with the reason, in every result and on every page that would show them |
| D-06 | no hosted CI | **Written; not yet run on GitHub** | `.github/workflows/ci.yml`: engine build + ctest, Python, API contract, libFuzzer, web lint/typecheck/tests/build, Playwright on OSM Monaco. Nothing has been pushed, so the hosted run is pending. Its steps were run locally on Linux (WSL Ubuntu 26.04): GCC 15.2 and Clang 21 Release `-Werror` and Clang Debug ASan+UBSan+LSan, each 847/847 with zero warnings; libFuzzer 60 s per target, no findings after defect 6's fix. The full stack also runs from `compose.yaml` (Playwright 5/5 against the containers) |
| D-07 | no formal fuzzing | **Resolved** | `cpp/fuzz/`: graph binary reader, model loader, experiment manifest reader, queue operation sequences; libFuzzer with clang, standalone smoke runs in ctest (`fuzz-smoke`). It found two real defects (1 and 6 below) |
| D-08 | checked-in / absolute-path build state | **Resolved** | `build/` is ignored; the local `build/release` was configured from a different absolute path and could not rebuild, so it was set aside and the release preset reconfigured from source |
| D-09 | expected outcome vs findings | **Visible** | Compare and Research never declare a winner; paired statistics report effect size and fraction of queries beside Holm-adjusted p-values; the revised abstract is [ABSTRACT.md](ABSTRACT.md) |
| D-10 | train/eval leakage history | **Automated** | `python/lpq_tools/training/leakage.py`; `test_leakage.py` asserts the committed NY training set is disjoint from both evaluation manifests; every model shows `Training/Evaluation split: DISJOINT` or a warning |

## Defects found while building LPQ Atlas

### 1. Bucket substrate returned keys out of order past its window (correctness)

`BucketSubstrate` parks keys beyond `base + 1024 × width` in an overflow list and
re-filed them only after a full lap of empty buckets. When the window slid forward
without such a lap, a parked key the window now covered stayed parked, later pushes with
larger keys went into buckets, and those were extracted first. In a search that is a
**non-optimal distance in exact mode** (NY, width 1: 1,154,372 returned vs 1,139,254
exact). Found by `fuzz_queue_ops` on its first run (seed 20260926); confirmed end-to-end
with `lpq_run --lpq-bucket-width 1|2|4`.

*Fix:* the substrate now keeps the invariant "every overflow key is at or beyond the
window end", re-filing covered entries whenever the base advances
(`absorb_overflow`). Regression tests: `AnOverflowKeyTheWindowCatchesUpWithIsNotOvertaken`,
`StaysOrderedWhenTheWidthIsFarBelowTheKeyRange` (both fail on the old code).

*Effect on stored results: none.* The window only overflows when the bucket width is
below max-weight / 1024. Every recorded experiment used the derived width (= ceil(max
weight / 1024)) or wider (500, 5,000 on NY; ≥ 5 on random graphs with weights ≤ 1000), so
no key ever exceeded the window. In addition, `lpq_benchmark` validates every query
against the reference and aborts on a mismatch, so no wrong distance could have been
recorded.

### 2. Overflow re-filing discarded learned scores

`redistribute_overflow` re-pushed entries without their score, silently resetting it to
zero. Fixed; regression test `RefilingFromTheOverflowListKeepsTheScore`. Same scope as
above: unreachable at the derived width.

### 3. Haversine A* was given the Euclidean scale

`lpq_benchmark --heuristic haversine` passed `euclidean_admissible_scale` (weight per
*degree*) to a heuristic that multiplies *metres*, overestimating by orders of magnitude
and breaking optimality. No recorded experiment used haversine (A* results used
`euclidean`). Added `haversine_admissible_scale` and used it in both runners.

### 4. DIMACS coordinates labelled "geographic" were microdegrees

`lpq_convert` stored `.co` values as-is (microdegrees) under `CoordinateSystem::kGeographic`,
whose contract is degrees. Haversine on those values stayed admissible (the scale is
derived) but collapsed toward zero, turning A* into Dijkstra. `lpq_convert` now converts
to degrees after any weight synthesis (so synthesized weights are unchanged);
`haversine_admissible_scale` refuses out-of-range coordinates. All coordinate datasets
were reconverted; their arc checksums are unchanged (coordinates are not in the
checksum) and their query manifests are byte-identical. Euclidean A* is scale-invariant
(its scale is derived from the same coordinates), so recorded A* results are unaffected
up to integer rounding of the heuristic.

### 5. Python header reader lagged the graph format

`binary_header.py` accepted only format version 2 after the C++ writer moved to version
3, so any newly converted graph failed the pipeline's header check. It now reads 2 and 3
and reports the coordinate system.

### 6. Binary graph reader trusted the header's counts (robustness)

`read_binary_graph` sized its offset array from the declared node count and reserved the
declared arc count before reading any payload. A 72-byte file declaring 654 million nodes
requested 5.2 GB (libFuzzer out-of-memory in `fuzz_graph_binary`, first libFuzzer run on
Linux, seed 20260926); an arc count near 2^64 escaped as `std::length_error` instead of
`GraphIoError`, bypassing every caller that catches `LpqError`.

*Fix:* the declared counts are checked against the bytes remaining in the stream before
any allocation; offsets must start at 0 and never decrease; a stream that cannot report
its size (a pipe) grows its arrays only as bytes arrive. Regression tests:
`AHeaderClaimingMoreNodesThanTheFileHoldsIsRejectedBeforeAllocating` (the fuzzer's input,
embedded) and `AnArcCountNearTwoToTheSixtyFourIsAGraphIoError`, both failing on the old
reader, plus two unseekable-stream tests.

*Effect on stored results: none.* Every graph the project loads was written by
`lpq_convert` and passes the new checks unchanged (all datasets load, all suites pass).
