"""Turning validated requests into engine executions (spec sections 7, 25, 41).

The C++ engine is the only thing that computes a distance, a path, a counter
or a runtime. This module builds its argv from validated enums, runs it as a
job, validates what it wrote, and never edits a number it returns.
"""

from __future__ import annotations

import json
import platform
import random
import secrets
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from lpq_tools.data.queries import build_manifest, read_manifest, write_manifest

from atlas_api import lanes as lane_scoring
from atlas_api.config import Settings
from atlas_api.jobs import (
    FINALIZING,
    RUNNING,
    Job,
    JobFailure,
    JobManager,
    run_engine,
)
from atlas_api.registry import QUEUES, Registry, engine_queue_ids
from atlas_api.schemas import (
    BenchmarkRequest,
    EngineConfig,
    LaneComparisonRequest,
    LaneSpec,
    RunRequest,
)

RUN_SCHEMA_VERSION = 1
LANE_SCHEMA_VERSION = 1

# One timed engine process at a time, across single runs and lane comparisons.
# Two timed searches overlapping would contend for memory bandwidth and caches
# and make both numbers meaningless, so timed work takes turns.
_TIMING_LOCK = threading.Lock()


@contextmanager
def timing_slot(job: Job, manager: JobManager) -> Iterator[None]:
    """Hold the exclusive timing slot, staying cancellable while waiting for it."""
    announced = False
    while not _TIMING_LOCK.acquire(timeout=0.25):
        manager.check_cancel(job)
        if not announced:
            manager.set_state(job, job.state, progress="waiting for an exclusive timing slot")
            announced = True
    try:
        yield
    finally:
        _TIMING_LOCK.release()


def _engine_flags(cfg: EngineConfig, registry: Registry) -> list[str]:
    argv = ["--algorithm", cfg.algorithm, "--pq", cfg.queue, "--mode", cfg.correctness_policy]
    if cfg.algorithm == "astar":
        argv += ["--heuristic", cfg.heuristic]
    if cfg.correctness_policy == "approximate":
        argv += ["--epsilon", repr(float(cfg.epsilon))]
    if cfg.model_id:
        argv += ["--model", str(registry.model_path(cfg.model_id))]
    if cfg.adaptive_learning:
        argv.append("--adaptive-learning")
    if cfg.lpq_window is not None:
        argv += ["--lpq-window", f"{int(cfg.lpq_window[0])}:{int(cfg.lpq_window[1])}"]
    if cfg.lpq_bucket_width is not None:
        argv += ["--lpq-bucket-width", str(int(cfg.lpq_bucket_width))]
    return argv


def validate_against_dataset(
    cfg: EngineConfig, dataset: dict[str, Any], registry: Registry
) -> None:
    """Checks that need the dataset: coordinates for A*, model compatibility."""
    if cfg.algorithm == "astar" and cfg.heuristic in ("auto", "haversine", "euclidean"):
        cs = dataset.get("coordinate_system")
        if cfg.heuristic == "auto" and cs not in ("geographic", "euclidean"):
            raise ValueError(
                "this dataset has no coordinates, so A* has no admissible geometric heuristic; "
                "choose the zero heuristic or Dijkstra"
            )
        if cfg.heuristic == "haversine" and cs != "geographic":
            raise ValueError("the haversine heuristic needs geographic coordinates")
        if cfg.heuristic == "euclidean" and cs != "euclidean":
            raise ValueError("the euclidean heuristic needs plane coordinates")
    if cfg.model_id:
        try:
            model = registry.model(cfg.model_id)
        except KeyError as e:
            raise ValueError(f"unknown model: {cfg.model_id}") from e
        if model.get("training_dataset") != dataset["id"]:
            raise ValueError(
                f"model {cfg.model_id} was trained on {model.get('training_dataset')}; its "
                f"static feature scaling is not validated for {dataset['id']}"
            )


def run_argv(
    settings: Settings, registry: Registry, req: RunRequest, graph: Path, run_dir: Path, run_id: str
) -> list[str]:
    max_events = min(req.max_events or settings.max_trace_events, settings.max_trace_events)
    argv = [
        str(settings.tool("lpq_run")),
        "--dataset",
        str(graph),
        "--source",
        str(int(req.source_node)),
        "--target",
        str(int(req.target_node)),
        *_engine_flags(req, registry),
        "--trace-level",
        req.trace_level,
        "--max-events",
        str(int(max_events)),
        "--repetitions",
        str(int(req.repetitions)),
        "--warmup",
        str(int(req.warmup)),
        "--run-id",
        run_id,
        "--result-out",
        str(run_dir / "result.json"),
    ]
    if req.trace_level != "none":
        argv += ["--trace-out", str(run_dir / "trace.jsonl")]
    return argv


def validate_result(result: dict[str, Any], run_id: str, expected_checksum: str) -> list[str]:
    """Structural checks on what the engine wrote. Returns problems; empty is valid."""
    problems = []
    if result.get("schema_version") != RUN_SCHEMA_VERSION:
        problems.append("unsupported result schema version")
    if result.get("run_id") != run_id:
        problems.append("result belongs to a different run")
    if result.get("status") == "completed":
        if result.get("dataset", {}).get("checksum_fnv1a64") != expected_checksum:
            problems.append("result was computed on a different graph")
        res = result.get("result", {})
        if res.get("reachable") and not res.get("path_cost_verified"):
            problems.append("returned path does not cost the returned distance")
        trace = result.get("trace", {})
        if trace.get("available") and not trace.get("matches_untraced_run"):
            problems.append("the traced run did not reproduce the untraced run")
    return problems


def execute_run(
    job: Job,
    manager: JobManager,
    settings: Settings,
    registry: Registry,
    req: RunRequest,
    graph: Path,
    checksum: str,
) -> dict[str, Any]:
    argv = run_argv(settings, registry, req, graph, job.dir, job.id)
    manager.set_state(job, RUNNING)
    if req.repetitions > 0:
        with timing_slot(job, manager):
            code = run_engine(job, manager, argv, settings.root, settings.run_timeout_s)
    else:
        code = run_engine(job, manager, argv, settings.root, settings.run_timeout_s)
    manager.set_state(job, FINALIZING)
    result_path = job.dir / "result.json"
    if not result_path.exists():
        raise JobFailure("engine", f"the engine exited with code {code} and wrote no result")
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except ValueError as e:
        raise JobFailure("malformed_artifact", "the engine wrote an unreadable result") from e
    if result.get("status") == "failed":
        err = result.get("error", {})
        raise JobFailure(err.get("category", "engine"), err.get("message", "engine failure"))
    problems = validate_result(result, job.id, checksum)
    if problems:
        raise JobFailure("validation", "; ".join(problems))
    trace = job.dir / "trace.jsonl"
    if trace.exists() and trace.stat().st_size > settings.max_trace_bytes:
        trace.unlink()
        result["trace"]["available"] = False
        result["trace"]["removed_reason"] = "trace exceeded the server's size cap"
        result_path.write_text(json.dumps(result, indent=1))
    r = result["result"]
    corr = result["correctness"]
    bench = result["timing"].get("benchmark")
    return {
        "dataset_id": req.dataset_id,
        "algorithm": req.algorithm,
        "queue": req.queue,
        "policy": req.correctness_policy,
        "reachable": r["reachable"],
        "distance": r.get("distance"),
        "reference_distance": result["reference"].get("distance"),
        "bound_holds": corr.get("bound_holds"),
        "median_ns": bench["median_ns"] if bench else None,
    }


# -----------------------------------------------------------------------------
# Lane comparison: several (algorithm, queue) lanes, timed one after another
# -----------------------------------------------------------------------------

_FAMILY = {q["id"]: q["family"] for q in QUEUES}
_COUNTER_KEYS = {
    "pops": "pq_pops",
    "pushes": "pq_pushes",
    "decrease_keys": "pq_decrease_keys",
    "comparisons": "pq_comparisons",
    "settled_nodes": "settled_nodes",
    "stale_pops": "stale_pops",
    "edges_examined": "edges_examined",
    "edges_relaxed": "edges_relaxed",
    "peak_queue_size": "peak_queue_size",
}


def resolve_shared_heuristic(requested: str, dataset: dict[str, Any]) -> str:
    """The one admissible heuristic every A* lane uses.

    With coordinates, the geometric heuristic that matches the graph's coordinate
    system (Haversine on longitude/latitude, Euclidean on a plane; both scaled by the
    engine to stay admissible and consistent for this graph's weights). Without
    coordinates, the zero heuristic.
    """
    if requested != "auto":
        return requested
    cs = dataset.get("coordinate_system")
    return "haversine" if cs == "geographic" else "euclidean" if cs == "euclidean" else "zero"


def lane_run_requests(
    req: LaneComparisonRequest, dataset: dict[str, Any]
) -> tuple[str, dict[str, RunRequest]]:
    """One RunRequest per lane with identical query, counts, tracing off. Raises ValueError."""
    heuristic = resolve_shared_heuristic(req.heuristic, dataset)
    out: dict[str, RunRequest] = {}
    for lane in req.lanes:
        out[lane.id] = RunRequest(
            dataset_id=req.dataset_id,
            source_node=req.source_node,
            target_node=req.target_node,
            algorithm=lane.algorithm,
            heuristic=heuristic if lane.algorithm == "astar" else "auto",
            queue=lane.queue,
            trace_level="none",
            repetitions=req.repetitions,
            warmup=req.warmup,
        )
    return heuristic, out


def _log_tail(path: Path) -> str:
    try:
        lines = [
            ln.strip()
            for ln in path.read_text(encoding="utf-8", errors="replace").splitlines()
            if ln.strip() and not ln.startswith("argv:")
        ]
    except OSError:
        return ""
    return lines[-1][:300] if lines else ""


def _lane_error(category: str, message: str) -> dict[str, Any]:
    return {"category": category, "message": message}


def _optimizer_view(result: dict[str, Any]) -> dict[str, Any] | None:
    """What the engine's `optimizer` block says the Optimized queue resolved to, if present."""
    o = result.get("optimizer")
    if not isinstance(o, dict):
        return None
    return {
        "resolved_queue": o.get("resolved_queue"),
        "resolved_kind": o.get("resolved_kind"),
        "resolution_method": o.get("resolution_method"),
        "calibration_ns": o.get("calibration_ns"),
        "reason": o.get("reason"),
        "predicted_ratio_vs_radix": o.get("predicted_ratio_vs_radix"),
        "tuned_candidates": o.get("tuned_candidates") or [],
    }


def _lane_from_result(result: dict[str, Any]) -> dict[str, Any]:
    """One round's facts: answer, counters and the engine's raw benchmark block."""
    r = result["result"]
    counters = result.get("counters") or {}
    available = bool(result.get("counters_available", True))
    ref = result["reference"]
    return {
        "reachable": r["reachable"],
        "distance": r.get("distance"),
        "path_hops": r.get("path_hops"),
        "path": r.get("path") or [],
        "reference": {"reachable": ref["reachable"], "distance": ref.get("distance")},
        "counts": {k: (counters.get(v) if available else None) for k, v in _COUNTER_KEYS.items()},
        "lpq": result.get("lpq"),
        "heuristic": result.get("heuristic"),
        "optimizer": _optimizer_view(result),
        "bench": result["timing"].get("benchmark"),
    }


def _run_lane(
    job: Job,
    manager: JobManager,
    settings: Settings,
    registry: Registry,
    spec: LaneSpec,
    rr: RunRequest,
    graph: Path,
    checksum: str,
    supported: frozenset[str] | None,
    tag: str,
    timed: bool = True,
) -> dict[str, Any]:
    """Run one lane once (one engine process). A failure is returned, never raised.

    `tag` names the sub-directory and log ("r1", "warmup"). With `timed` False the
    request has no repetitions, so the process only resolves the queue and searches once.
    """
    if supported is not None and spec.queue not in supported:
        return {
            "status": "error",
            "error": _lane_error(
                "queue_unavailable",
                f"this engine build does not provide the '{spec.queue}' queue; rebuild lpq_run "
                "with it to include this lane",
            ),
        }
    lane_dir = job.dir / spec.id / tag
    lane_dir.mkdir(parents=True, exist_ok=True)
    run_id = f"{job.id}_{spec.id}_{tag}"
    log_name = f"{spec.id}_{tag}.log"
    argv = run_argv(settings, registry, rr, graph, lane_dir, run_id)
    try:
        code = run_engine(
            job, manager, argv, settings.root, settings.run_timeout_s, log_name=log_name
        )
    except JobFailure as e:  # timeout
        return {"status": "error", "error": _lane_error(e.category, e.message)}
    result_path = lane_dir / "result.json"
    if not result_path.exists():
        detail = _log_tail(job.dir / log_name)
        msg = f"the engine exited with code {code} and wrote no result"
        return {
            "status": "error",
            "error": _lane_error("engine", f"{msg}: {detail}" if detail else msg),
        }
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except ValueError:
        return {
            "status": "error",
            "error": _lane_error("malformed_artifact", "the engine wrote an unreadable result"),
        }
    if result.get("status") == "failed":
        err = result.get("error", {})
        return {
            "status": "error",
            "error": _lane_error(
                err.get("category", "engine"), err.get("message", "engine failure")
            ),
        }
    problems = validate_result(result, run_id, checksum)
    if problems:
        return {"status": "error", "error": _lane_error("validation", "; ".join(problems))}
    lane = _lane_from_result(result)
    if timed and lane["bench"] is None:
        return {"status": "error", "error": _lane_error("engine", "the engine returned no timing")}
    lane["status"] = "ok"
    lane["error"] = None
    return lane


def merge_lane_rounds(
    warmup: dict[str, Any] | None, rounds: list[dict[str, Any]]
) -> dict[str, Any]:
    """Fold one lane's untimed warm-up call and its per-round runs into the lane record.

    Pure (no engine, no I/O). `rounds` has one entry per round, in round order, each
    either an ok run from `_run_lane` or an error run. The warm-up is never part of the
    timing: it exists so a one-time cost (the Optimized queue's calibration) cannot land
    in a timed round, and its calibration time is reported separately as one-time setup.
    """
    ok = [r for r in rounds if r["status"] == "ok"]
    if not ok:
        first = next((r for r in rounds if r["status"] != "ok"), None)
        err = (first or {}).get("error") or _lane_error("engine", "no round produced a timing")
        return {"status": "error", "error": err, "timing": None}
    timing = lane_scoring.aggregate_rounds(
        [r["bench"] if r["status"] == "ok" else None for r in rounds]
    )
    assert timing is not None  # at least one ok round, checked above
    base = ok[0]
    lane: dict[str, Any] = {
        k: base[k]
        for k in ("reachable", "distance", "path_hops", "reference", "counts", "lpq", "heuristic")
    }
    lane["_path"] = base["path"]
    lane["status"] = "ok"
    lane["error"] = None
    lane["timing"] = timing
    lane["median_ns"] = timing["median_ns"]
    lane["mad_ns"] = timing["mad_ns"]
    lane["failed_rounds"] = [i + 1 for i, r in enumerate(rounds) if r["status"] != "ok"]
    if lane["failed_rounds"]:
        timing["warnings"].append(
            f"round(s) {', '.join(map(str, lane['failed_rounds']))} failed and were left out"
        )
    # Every round must have found the same answer.
    if len({(r["reachable"], r["distance"]) for r in ok}) > 1:
        return {
            "status": "error",
            "error": _lane_error("validation", "the distance differed between rounds"),
            "timing": None,
        }
    opt = (warmup or {}).get("optimizer") if warmup and warmup["status"] == "ok" else None
    opt = opt or base.get("optimizer")
    lane["resolved_queue"] = lane["resolved_kind"] = lane["resolution_method"] = None
    lane["calibration_ns"] = None
    lane["resolution_reason"] = None
    lane["predicted_ratio_vs_radix"] = None
    lane["resolved_to_radix"] = False
    if opt:
        lane.update(
            resolved_queue=opt["resolved_queue"],
            resolved_kind=opt["resolved_kind"],
            resolution_method=opt["resolution_method"],
            calibration_ns=opt["calibration_ns"],
            resolution_reason=opt["reason"],
            predicted_ratio_vs_radix=opt["predicted_ratio_vs_radix"],
            tuned_candidates=opt["tuned_candidates"],
            resolved_to_radix=opt["resolved_kind"] == "radix_heap",
        )
        # Calibration inside a timed round would inflate that round; say so loudly.
        late = [i + 1 for i, r in enumerate(rounds) if r["status"] == "ok"
                and ((r.get("optimizer") or {}).get("calibration_ns") or 0) > 0]
        if late:
            timing["warnings"].append(
                f"queue calibration ran during timed round(s) {', '.join(map(str, late))}; "
                "those rounds include set-up cost"
            )
    return lane


def execute_lane_comparison(
    job: Job,
    manager: JobManager,
    settings: Settings,
    registry: Registry,
    req: LaneComparisonRequest,
    dataset: dict[str, Any],
    graph: Path,
    other_timed_work: Callable[[], bool] = lambda: False,
) -> dict[str, Any]:
    """Time every lane on one query in several rounds, sequentially, in shuffled orders.

    Each (lane, round) is one lpq_run process: it loads the graph, pins itself to a core,
    runs `warmup` discarded searches and `repetitions` timed ones with tracing off.
    Round r runs EVERY lane once in an order shuffled with seed (order_seed + r). Processes
    never overlap -- not with each other, and not with single runs (they share the timing
    slot). Before round 1 each Optimized lane gets one untimed call, so its one-time
    calibration (cached afterwards) can never land in a timed round.
    """
    heuristic, run_reqs = lane_run_requests(req, dataset)
    seed = req.order_seed if req.order_seed is not None else secrets.randbelow(2**32)
    ids = [lane.id for lane in req.lanes]
    round_orders: list[list[str]] = []
    for r in range(1, req.rounds + 1):
        order = list(ids)
        random.Random(seed + r).shuffle(order)
        round_orders.append(order)
    specs = {lane.id: lane for lane in req.lanes}
    supported = engine_queue_ids(settings)
    checksum = dataset["checksum_fnv1a64"]

    warmups: dict[str, dict[str, Any]] = {}
    runs: dict[str, list[dict[str, Any]]] = {i: [] for i in ids}
    contended: set[str] = set()
    started = time.time()
    total = len(ids) * req.rounds
    done = 0
    with timing_slot(job, manager):
        for lane_id in ids:
            spec = specs[lane_id]
            if spec.queue != "lpq_optimized":
                continue
            manager.check_cancel(job)
            manager.set_state(
                job, RUNNING, progress=f"preparing {spec.label or spec.id} (untimed set-up call)"
            )
            untimed = run_reqs[lane_id].model_copy(update={"repetitions": 0, "warmup": 0})
            warmups[lane_id] = _run_lane(
                job, manager, settings, registry, spec, untimed, graph, checksum, supported,
                "warmup", timed=False,
            )
        for r, order in enumerate(round_orders, start=1):
            for lane_id in order:
                manager.check_cancel(job)
                spec = specs[lane_id]
                done += 1
                manager.set_state(
                    job,
                    RUNNING,
                    progress=f"round {r} of {req.rounds}, timing {spec.label or spec.id} "
                    f"({done} of {total})",
                )
                busy = other_timed_work()
                runs[lane_id].append(
                    _run_lane(
                        job, manager, settings, registry, spec, run_reqs[lane_id], graph,
                        checksum, supported, f"r{r}",
                    )
                )
                if busy or other_timed_work():
                    contended.add(lane_id)
    manager.set_state(job, FINALIZING)

    lanes: list[dict[str, Any]] = []
    path: list[int] = []
    for spec in req.lanes:  # response lanes keep the request order
        # Rounds run in order, so runs[id][i] is round i + 1 for every lane.
        merged = merge_lane_rounds(warmups.get(spec.id), runs[spec.id])
        lane = {
            "id": spec.id,
            "label": spec.label,
            "algorithm": spec.algorithm,
            "queue": spec.queue,
            "family": _FAMILY.get(spec.queue),
            "run_positions": [order.index(spec.id) + 1 for order in round_orders],
            "run_id": f"{job.id}_{spec.id}",
            **{k: v for k, v in merged.items() if k != "_path"},
        }
        if merged["status"] == "ok":
            lane["heuristic_used"] = (lane.pop("heuristic") or {}).get("name")
            if not path and merged["_path"]:
                path = merged["_path"]
            if spec.id in contended:
                lane["timing"]["warnings"] = [
                    *lane["timing"]["warnings"],
                    "another timed benchmark was running on this machine while this lane ran",
                ]
        lanes.append(lane)

    summary = lane_scoring.analyse(lanes)
    ok_lanes = [ln for ln in lanes if ln["status"] == "ok"]
    cores = {ln["timing"]["core"] for ln in ok_lanes}
    setup_ns = sum(ln.get("calibration_ns") or 0 for ln in ok_lanes)
    result = {
        "schema_version": LANE_SCHEMA_VERSION,
        "dataset_id": req.dataset_id,
        "source_node": req.source_node,
        "target_node": req.target_node,
        "measurement": {
            "rounds": req.rounds,
            "repetitions": req.repetitions,
            "warmup": req.warmup,
            "tracing": "off",
            "execution": "sequential: one engine process per lane per round, one at a time",
            "round_orders": round_orders,
            "order_seed": seed,
            "order_seed_rule": "round r is shuffled with seed order_seed + r",
            "all_pinned": all(ln["timing"]["pinned"] for ln in ok_lanes),
            "cores": sorted(c for c in cores if c is not None),
            "clock": "wall-clock around one search; graph load, queue set-up, queue calibration "
            "and the reference check are outside the clock",
            "aggregation": "lane median = median of the per-round medians; spread = larger of "
            "the median within-process MAD and the MAD of the per-round medians",
            "machine": {"os": platform.system(), "arch": platform.machine()},
            "elapsed_s": round(time.time() - started, 3),
            "contended_lanes": sorted(contended),
            "one_time_setup_ns": setup_ns,
        },
        "heuristic": {
            "requested": req.heuristic,
            "used_by_astar_lanes": heuristic
            if any(s.algorithm == "astar" for s in req.lanes)
            else None,
            "shared": True,
        },
        "lanes": lanes,
        "path": path,
        **summary,
    }
    (job.dir / "comparison.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    return {
        "dataset_id": req.dataset_id,
        "lanes_ok": len(ok_lanes),
        "lanes_failed": len(lanes) - len(ok_lanes),
    }


# -----------------------------------------------------------------------------
# Benchmarks: the existing lpq_benchmark harness, one process for the matrix
# -----------------------------------------------------------------------------


def execute_benchmark(
    job: Job,
    manager: JobManager,
    settings: Settings,
    registry: Registry,
    req: BenchmarkRequest,
    dataset: dict[str, Any],
) -> dict[str, Any]:
    ds = req.dataset_id
    # The query set: an existing manifest (resolved against the directory
    # listing), or a fresh one from the project's own deterministic generator.
    if req.query_manifest:
        src = registry.manifest_path(ds, req.query_manifest)
        manifest = read_manifest(src)
        pairs = manifest.pairs[: req.query_count]
        manifest = build_manifest(
            ds,
            manifest.node_count,
            count=len(pairs),
            seed=manifest.seed,
            graph_checksum=manifest.graph_checksum,
        )
        if manifest.pairs != pairs:
            # A prefix of a generated manifest is the generator's output for a
            # smaller count only if the generator is prefix-stable; check it.
            raise JobFailure("invalid_request", "cannot take a stable prefix of that manifest")
        query_source = {"manifest": src.name, "prefix": len(pairs), "seed": manifest.seed}
    else:
        manifest = build_manifest(
            ds,
            int(dataset["nodes"]),
            count=req.query_count,
            seed=req.seed,
            graph_checksum=int(dataset["checksum_fnv1a64"]),
        )
        query_source = {"generated": True, "seed": req.seed, "count": req.query_count}
    queries_path = job.dir / "queries.yaml"
    write_manifest(queries_path, manifest)

    experiment_id = f"atlas_{job.id}"
    # Written by hand, not yaml.safe_dump: experiment_manifest.hpp reads a
    # strict YAML subset and does not unquote values. Every value here is an
    # enum, a number or a path.
    try:
        rel_queries = queries_path.resolve().relative_to(settings.root.resolve()).as_posix()
    except ValueError:
        # A work directory outside the repository: an absolute path is fine,
        # the reader splits a line at its first colon only.
        rel_queries = queries_path.resolve().as_posix()
    lines = [
        f"experiment: {experiment_id}",
        f"dataset: {ds}",
        f"queries_file: {rel_queries}",
        "algorithm:",
        f"  - {req.algorithm}",
        "pq:",
        *[f"  - {q}" for q in req.queues],
        "mode:",
        f"  - {req.correctness_policy}",
        f"query_count: {len(manifest.pairs)}",
        f"repetitions: {int(req.repetitions)}",
        f"warmup: {int(req.warmup)}",
        f"seed: {int(manifest.seed)}",
        f"heuristic: {req.heuristic}",
    ]
    if req.correctness_policy == "approximate":
        lines.append(f"epsilon: {float(req.epsilon)!r}")
    lines += [
        "collect:",
        "  runtime: true",
        "  pq_operations: true",
        "  ml_latency: false",
        "  hardware_counters: false",
    ]
    exp_path = job.dir / "experiment.yaml"
    exp_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    out = job.dir / "results.json"
    argv = [
        str(settings.tool("lpq_benchmark")),
        "--experiment",
        str(exp_path),
        "--output",
        str(out),
    ]
    if req.model_id:
        argv += ["--model", str(registry.model_path(req.model_id))]
    manager.set_state(
        job, RUNNING, progress=f"{len(req.queues)} queue(s) x {len(manifest.pairs)} queries"
    )
    code = run_engine(job, manager, argv, settings.root, settings.benchmark_timeout_s)
    manager.set_state(job, FINALIZING)
    if code != 0 or not out.exists():
        tail = (job.dir / "engine.log").read_text(encoding="utf-8", errors="replace")[-600:]
        reason = "a correctness check failed" if code == 2 else f"exit code {code}"
        raise JobFailure("engine", f"lpq_benchmark failed ({reason}): {tail.strip()[-300:]}")
    return {
        "dataset_id": ds,
        "queues": list(req.queues),
        "queries": query_source,
        "experiment_id": experiment_id,
    }
