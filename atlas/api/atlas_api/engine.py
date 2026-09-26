"""Turning validated requests into engine executions (spec sections 7, 25, 41).

The C++ engine is the only thing that computes a distance, a path, a counter
or a runtime. This module builds its argv from validated enums, runs it as a
job, validates what it wrote, and never edits a number it returns.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from atlas_api.config import Settings
from atlas_api.jobs import FINALIZING, RUNNING, Job, JobFailure, JobManager, run_engine
from atlas_api.registry import Registry
from atlas_api.schemas import BenchmarkRequest, EngineConfig, RunRequest
from lpq_tools.data.queries import build_manifest, read_manifest, write_manifest

RUN_SCHEMA_VERSION = 1


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
