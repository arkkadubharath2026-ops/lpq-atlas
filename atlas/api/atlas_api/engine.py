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


def _lane_from_result(spec: LaneSpec, result: dict[str, Any]) -> dict[str, Any]:
    r, bench = result["result"], result["timing"].get("benchmark")
    counters = result.get("counters") or {}
    available = bool(result.get("counters_available", True))
    ref = result["reference"]
    lane: dict[str, Any] = {
        "reachable": r["reachable"],
        "distance": r.get("distance"),
        "path_hops": r.get("path_hops"),
        "reference": {"reachable": ref["reachable"], "distance": ref.get("distance")},
        "counts": {
            k: (counters.get(v) if available else None) for k, v in _COUNTER_KEYS.items()
        },
        "lpq": result.get("lpq"),
        "heuristic": result.get("heuristic"),
        "timing": None,
    }
    if bench:
        lane["timing"] = {
            "median_ns": bench["median_ns"],
            "mad_ns": bench["mad_ns"],
            "relative_mad": bench.get("relative_mad"),
            "min_ns": bench.get("min_ns"),
            "max_ns": bench.get("max_ns"),
            "samples_ns": bench.get("samples_ns", []),
            "repetitions": bench.get("repetitions"),
            "warmup": bench.get("warmup"),
            "noisy": bench.get("noisy"),
            "pinned": bench.get("pinned"),
            "core": bench.get("core"),
            "warnings": bench.get("warnings", []),
        }
    return lane


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
) -> dict[str, Any]:
    """Run one lane. A failure is returned as the lane's own error, never raised."""
    if supported is not None and spec.queue not in supported:
        return {
            "status": "error",
            "error": _lane_error(
                "queue_unavailable",
                f"this engine build does not provide the '{spec.queue}' queue; rebuild lpq_run "
                "with it to include this lane",
            ),
        }
    lane_dir = job.dir / spec.id
    lane_dir.mkdir(exist_ok=True)
    run_id = f"{job.id}_{spec.id}"
    argv = run_argv(settings, registry, rr, graph, lane_dir, run_id)
    try:
        code = run_engine(
            job, manager, argv, settings.root, settings.run_timeout_s, log_name=f"{spec.id}.log"
        )
    except JobFailure as e:  # timeout
        return {"status": "error", "error": _lane_error(e.category, e.message)}
    result_path = lane_dir / "result.json"
    if not result_path.exists():
        detail = _log_tail(job.dir / f"{spec.id}.log")
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
            "error": _lane_error(err.get("category", "engine"), err.get("message", "engine failure")),
        }
    problems = validate_result(result, run_id, checksum)
    if problems:
        return {"status": "error", "error": _lane_error("validation", "; ".join(problems))}
    lane = _lane_from_result(spec, result)
    if lane["timing"] is None:
        return {"status": "error", "error": _lane_error("engine", "the engine returned no timing")}
    lane["status"] = "ok"
    lane["error"] = None
    # Flat copies of the two headline timing figures, for consumers that only want those.
    lane["median_ns"] = lane["timing"]["median_ns"]
    lane["mad_ns"] = lane["timing"]["mad_ns"]
    lane["_path"] = result["result"].get("path") or []
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
    """Time every lane on one query, sequentially, in a per-request shuffled order.

    Each lane is one lpq_run process: it loads the graph, pins itself to a core,
    runs `warmup` discarded searches and `repetitions` timed ones with tracing off,
    and reports median and spread. Processes never overlap -- not with each other,
    and not with single runs (they share the timing slot).
    """
    heuristic, run_reqs = lane_run_requests(req, dataset)
    seed = req.order_seed if req.order_seed is not None else secrets.randbelow(2**32)
    order = [lane.id for lane in req.lanes]
    random.Random(seed).shuffle(order)
    specs = {lane.id: lane for lane in req.lanes}
    supported = engine_queue_ids(settings)
    checksum = dataset["checksum_fnv1a64"]

    runs: dict[str, dict[str, Any]] = {}
    contended: list[str] = []
    started = time.time()
    with timing_slot(job, manager):
        for i, lane_id in enumerate(order, start=1):
            manager.check_cancel(job)
            spec = specs[lane_id]
            manager.set_state(
                job, RUNNING, progress=f"timing lane {i} of {len(order)}: {spec.label or spec.id}"
            )
            busy_before = other_timed_work()
            runs[lane_id] = _run_lane(
                job, manager, settings, registry, spec, run_reqs[lane_id], graph, checksum, supported
            )
            if busy_before or other_timed_work():
                contended.append(lane_id)
    manager.set_state(job, FINALIZING)

    lanes: list[dict[str, Any]] = []
    path: list[int] = []
    for spec in req.lanes:  # response lanes keep the request order; `order` is the run order
        run = runs[spec.id]
        lane = {
            "id": spec.id,
            "label": spec.label,
            "algorithm": spec.algorithm,
            "queue": spec.queue,
            "family": _FAMILY.get(spec.queue),
            "run_index": order.index(spec.id) + 1,
            "run_id": f"{job.id}_{spec.id}",
            **{k: v for k, v in run.items() if k != "_path"},
        }
        if run["status"] == "ok":
            lane["heuristic_used"] = (lane.pop("heuristic") or {}).get("name")
            if not path and run["_path"]:
                path = run["_path"]
            if spec.id in contended:
                lane["timing"]["warnings"] = [
                    *lane["timing"]["warnings"],
                    "another timed benchmark was running on this machine while this lane ran",
                ]
        else:
            lane.setdefault("timing", None)
        lanes.append(lane)

    summary = lane_scoring.analyse(lanes)
    pinned = {ln["timing"]["core"] for ln in lanes if ln["status"] == "ok"}
    result = {
        "schema_version": LANE_SCHEMA_VERSION,
        "dataset_id": req.dataset_id,
        "source_node": req.source_node,
        "target_node": req.target_node,
        "measurement": {
            "repetitions": req.repetitions,
            "warmup": req.warmup,
            "tracing": "off",
            "execution": "sequential: one engine process per lane, one lane at a time",
            "run_order": order,
            "order_seed": seed,
            "all_pinned": all(ln["timing"]["pinned"] for ln in lanes if ln["status"] == "ok"),
            "cores": sorted(c for c in pinned if c is not None),
            "clock": "wall-clock around one search; graph load, queue set-up and the reference "
            "check are outside the clock",
            "machine": {"os": platform.system(), "arch": platform.machine()},
            "elapsed_s": round(time.time() - started, 3),
            "contended_lanes": contended,
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
        "lanes_ok": sum(1 for ln in lanes if ln["status"] == "ok"),
        "lanes_failed": sum(1 for ln in lanes if ln["status"] != "ok"),
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
