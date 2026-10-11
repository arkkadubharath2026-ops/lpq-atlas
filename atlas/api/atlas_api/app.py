"""LPQ Atlas orchestration API (spec section 19).

    uvicorn atlas_api.app:app --app-dir atlas/api --port 8000

The API validates, schedules and serves. It never computes a distance, path,
counter or runtime itself: those come from the C++ engine's result files, and
the trace it streams is the engine's own event log.
"""

from __future__ import annotations

import asyncio
import json
import logging
import subprocess
import time
from pathlib import Path
from typing import Any

import numpy as np
from atlas_api import analysis, graphcache
from atlas_api.config import get_settings
from atlas_api.engine import (
    execute_benchmark,
    execute_lane_comparison,
    execute_run,
    lane_run_requests,
    validate_against_dataset,
)
from atlas_api.jobs import (
    COMPLETED,
    FINALIZING,
    RUNNING,
    TERMINAL,
    Job,
    JobFailure,
    JobManager,
)
from atlas_api.logging_setup import configure_logging
from atlas_api.registry import (
    ALGORITHMS,
    Registry,
    capabilities,
    queues_with_availability,
)
from atlas_api.schemas import (
    BenchmarkRequest,
    ComparisonRequest,
    LaneComparisonRequest,
    RunRequest,
    SnapRequest,
)
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi import Path as ApiPath
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse, Response, StreamingResponse
from lpq_tools.data.pipeline import PipelineError, prepare_dataset
from lpq_tools.data.sources import load_dataset_config

configure_logging()
log = logging.getLogger("atlas.api")

settings = get_settings()
registry = Registry(settings)
graphs = graphcache.GraphCache(settings)
runs = JobManager(settings.runs_dir, "run", settings.max_concurrent_jobs)
# Separate pools: a comparison waits on two runs, so sharing the run pool could
# deadlock with every worker waiting for work that has no worker to run it.
comparisons = JobManager(settings.comparisons_dir, "comparison", 2)
benchmarks = JobManager(settings.benchmarks_dir, "benchmark", 1)
# One lane comparison at a time: its lanes are timed, and timed work must not overlap.
lane_comparisons = JobManager(settings.work_dir / "lane_comparisons", "lane_comparison", 1)
preparations = JobManager(settings.work_dir / "preparations", "prepare", 1)

app = FastAPI(title="LPQ Atlas API", version="1.0.0")


@app.middleware("http")
async def _request_log(request: Request, call_next: Any) -> Any:
    started = time.perf_counter()
    response = await call_next(request)
    log.info(
        "request",
        extra={
            "method": request.method,
            "path": request.url.path,
            "status": response.status_code,
            "ms": round((time.perf_counter() - started) * 1000, 1),
        },
    )
    return response


@app.exception_handler(RequestValidationError)
async def _validation(_: Request, exc: RequestValidationError) -> JSONResponse:
    # Only the message and field location: never the submitted values or paths.
    errors = [
        {"field": ".".join(str(p) for p in e.get("loc", [])[1:]), "message": e.get("msg", "")}
        for e in exc.errors()
    ]
    return JSONResponse(status_code=422, content={"error": "invalid_request", "details": errors})


def _bad(message: str, status: int = 400) -> HTTPException:
    return HTTPException(
        status_code=status, detail={"error": "invalid_request", "message": message}
    )


def _ready_dataset(dataset_id: str) -> tuple[dict[str, Any], Path]:
    try:
        ds = registry.dataset(dataset_id)
    except KeyError as e:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown dataset"}) from e
    if not ds.ready or ds.graph_path is None:
        raise HTTPException(
            409,
            detail={
                "error": "dataset_not_ready",
                "status": ds.status,
                "message": f"dataset {dataset_id} is {ds.status}",
            },
        )
    return ds.info, ds.graph_path


def _arrays(dataset_id: str) -> graphcache.GraphArrays:
    info, path = _ready_dataset(dataset_id)
    try:
        return graphs.get(dataset_id, path, info["checksum_fnv1a64"])
    except graphcache.GraphUnavailable as e:
        raise HTTPException(503, detail={"error": "graph_unavailable", "message": str(e)}) from e


def _job_or_404(manager: JobManager, job_id: str) -> Job:
    try:
        return manager.get(job_id)
    except KeyError as e:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown id"}) from e


def _read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise HTTPException(
            500, detail={"error": "malformed_artifact", "message": "stored artifact unreadable"}
        ) from e


# -----------------------------------------------------------------------------
# Metadata
# -----------------------------------------------------------------------------


@app.get("/api/health")
def health() -> dict[str, Any]:
    return {"status": "ok", "capabilities": capabilities(settings)["engine_tools"]}


@app.get("/api/datasets")
def list_datasets() -> list[dict[str, Any]]:
    return [d.info for d in registry.datasets()]


@app.get("/api/datasets/{dataset_id}")
def get_dataset(dataset_id: str) -> dict[str, Any]:
    try:
        return registry.dataset(dataset_id).info
    except KeyError as e:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown dataset"}) from e


@app.post("/api/datasets/{dataset_id}/prepare", status_code=202)
def prepare(dataset_id: str = ApiPath(pattern=r"^[a-z0-9_]{1,64}$")) -> dict[str, Any]:
    cfg_path = settings.root / "configs" / "datasets" / f"{dataset_id}.yaml"
    if not cfg_path.exists():
        raise HTTPException(404, detail={"error": "not_found", "message": "no dataset config"})
    config = load_dataset_config(cfg_path)

    def work(job: Job, m: JobManager) -> dict[str, Any]:
        def progress(msg: str) -> None:
            stage = (
                "DOWNLOADING"
                if "acquiring" in msg
                else "VALIDATING"
                if ("validating" in msg or "extracting" in msg)
                else "CONVERTING"
                if "converting" in msg
                else "PREPARING"
            )
            m.set_state(job, RUNNING, progress=f"{stage}: {msg.split(': ', 1)[-1]}")

        try:
            r = prepare_dataset(config, settings.paths, progress=progress)
        except (PipelineError, OSError, ValueError) as e:
            raise JobFailure("preparation", str(e).split("\n")[0][:300]) from e
        return {"dataset_id": dataset_id, "nodes": r.header.node_count, "converted": r.converted}

    job = preparations.submit({"dataset_id": dataset_id}, work, {"dataset_id": dataset_id})
    return job.public()


@app.get("/api/preparations/{job_id}")
def get_preparation(job_id: str) -> dict[str, Any]:
    return _job_or_404(preparations, job_id).public()


@app.get("/api/algorithms")
def list_algorithms() -> list[dict[str, Any]]:
    return ALGORITHMS


@app.get("/api/queues")
def list_queues() -> list[dict[str, Any]]:
    return queues_with_availability(settings)


@app.get("/api/models")
def list_models() -> list[dict[str, Any]]:
    return registry.models()


@app.get("/api/models/{model_id}")
def get_model(model_id: str) -> dict[str, Any]:
    try:
        return registry.model(model_id)
    except KeyError as e:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown model"}) from e


@app.get("/api/models/{model_id}/nodes")
def model_node_scores(
    model_id: str, nodes: str = Query(pattern=r"^\d{1,10}(,\d{1,10}){0,49}$")
) -> dict[str, Any]:
    """Static features and the queue's score for chosen nodes, computed by the C++ scorer."""
    try:
        model = registry.model(model_id)
        model_path = registry.model_path(model_id)
    except KeyError as e:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown model"}) from e
    ds_id = model.get("training_dataset")
    if not ds_id:
        raise _bad("the model's report does not name its dataset")
    _, graph = _ready_dataset(ds_id)
    proc = subprocess.run(  # noqa: S603 -- registry paths and a regex-validated id list
        [
            str(settings.tool("lpq_model_info")),
            "--model",
            str(model_path),
            "--dataset",
            str(graph),
            "--nodes",
            nodes,
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    if proc.returncode != 0:
        raise _bad(proc.stderr.strip().removeprefix("error: ")[:200] or "scoring failed")
    out: dict[str, dict[str, float]] = {}
    for line in proc.stdout.splitlines():
        if line.startswith("node."):
            key, _, val = line.partition(": ")
            _, node, name = key.split(".", 2)
            out.setdefault(node, {})[name] = float(val)
    return {
        "model_id": model_id,
        "dataset_id": ds_id,
        "score_semantics": "logistic model output (logit) stored per node at push time; the "
        "candidate window prefers higher scores",
        "nodes": out,
    }


@app.get("/api/capabilities")
def get_capabilities() -> dict[str, Any]:
    return capabilities(settings)


# -----------------------------------------------------------------------------
# Graph / map
# -----------------------------------------------------------------------------


@app.post("/api/graphs/{dataset_id}/snap")
def snap(dataset_id: str, body: SnapRequest) -> dict[str, Any]:
    g = _arrays(dataset_id)
    try:
        return graphcache.snap(g, body.lon, body.lat)
    except graphcache.GraphUnavailable as e:
        raise _bad(str(e)) from e


@app.get("/api/graphs/{dataset_id}/viewport")
def get_viewport(
    dataset_id: str,
    bbox: str = Query(pattern=r"^-?[\d.]+,-?[\d.]+,-?[\d.]+,-?[\d.]+$"),
) -> dict[str, Any]:
    w, s, e, n = (float(x) for x in bbox.split(","))
    if not (w < e and s < n):
        raise _bad("bbox must be west,south,east,north")
    g = _arrays(dataset_id)
    try:
        return graphcache.viewport(g, (w, s, e, n), settings.max_viewport_edges)
    except graphcache.GraphUnavailable as e:
        raise _bad(str(e)) from e


@app.get("/api/graphs/{dataset_id}/coords")
def get_coords(dataset_id: str) -> Response:
    """All node coordinates as little-endian float32 (lon, lat) pairs, indexed by node id."""
    g = _arrays(dataset_id)
    if g.coords is None:
        raise _bad("this dataset has no coordinates")
    return Response(
        content=g.coords.astype("<f4").tobytes(),
        media_type="application/octet-stream",
        headers={"X-Node-Count": str(g.nodes), "Cache-Control": "max-age=3600"},
    )


@app.get("/api/graphs/{dataset_id}/nodes/{node}")
def get_node(dataset_id: str, node: int) -> dict[str, Any]:
    g = _arrays(dataset_id)
    if not 0 <= node < g.nodes:
        raise _bad("node id out of range")
    lo, hi = int(g.offsets[node]), int(g.offsets[node + 1])
    out: dict[str, Any] = {
        "node": node,
        "out_arcs": [
            {"to": int(t), "weight": int(w)}
            for t, w in zip(g.targets[lo:hi], g.weights[lo:hi], strict=True)
        ],
    }
    if g.coords is not None:
        out["lon"], out["lat"] = float(g.coords[node, 0]), float(g.coords[node, 1])
    if g.osm_ids is not None:
        out["osm_node_id"] = int(g.osm_ids[node])
    return out


@app.get("/api/graphs/{dataset_id}/random-pair")
def random_pair(dataset_id: str, seed: int = Query(0, ge=0, le=2**31)) -> dict[str, Any]:
    """A source/target pair drawn with a seed, for datasets without a map."""
    g = _arrays(dataset_id)
    rng = np.random.default_rng(seed)
    s, t = (int(x) for x in rng.integers(0, g.nodes, size=2))
    return {"source_node": s, "target_node": t, "seed": seed}


# -----------------------------------------------------------------------------
# Runs
# -----------------------------------------------------------------------------


def _submit_run(req: RunRequest) -> Job:
    info, graph = _ready_dataset(req.dataset_id)
    if req.source_node >= int(info["nodes"]) or req.target_node >= int(info["nodes"]):
        raise _bad(f"source and target must be node ids below {info['nodes']}")
    try:
        validate_against_dataset(req, info, registry)
    except ValueError as e:
        raise _bad(str(e)) from e
    checksum = info["checksum_fnv1a64"]

    def work(job: Job, m: JobManager) -> dict[str, Any]:
        return execute_run(job, m, settings, registry, req, graph, checksum)

    return runs.submit(
        req.model_dump(mode="json"),
        work,
        {"dataset_id": req.dataset_id, "algorithm": req.algorithm, "queue": req.queue},
    )


@app.post("/api/runs", status_code=202)
def create_run(req: RunRequest) -> dict[str, Any]:
    return _submit_run(req).public()


@app.get("/api/runs")
def list_runs(limit: int = Query(50, ge=1, le=500)) -> list[dict[str, Any]]:
    return [j.public() for j in runs.list(limit)]


@app.get("/api/runs/{run_id}")
def get_run(run_id: str) -> dict[str, Any]:
    job = _job_or_404(runs, run_id)
    out = job.public()
    out["request"] = _read_json(job.dir / "request.json")
    out["has_result"] = (job.dir / "result.json").exists() and job.state == COMPLETED
    out["has_trace"] = (job.dir / "trace.jsonl").exists()
    return out


@app.post("/api/runs/{run_id}/cancel")
def cancel_run(run_id: str) -> dict[str, Any]:
    _job_or_404(runs, run_id)
    return runs.cancel(run_id).public()


@app.get("/api/runs/{run_id}/result")
def get_run_result(run_id: str) -> dict[str, Any]:
    job = _job_or_404(runs, run_id)
    if job.state != COMPLETED:
        raise HTTPException(
            409,
            detail={
                "error": "not_completed",
                "state": job.state,
                "message": (job.error or {}).get("message"),
            },
        )
    return _read_json(job.dir / "result.json")


def _trace_path(job: Job) -> Path:
    p = job.dir / "trace.jsonl"
    if not p.exists():
        raise HTTPException(
            404, detail={"error": "trace_unavailable", "message": "this run has no trace"}
        )
    return p


@app.get("/api/runs/{run_id}/trace")
def get_run_trace(run_id: str) -> FileResponse:
    job = _job_or_404(runs, run_id)
    if job.state != COMPLETED:
        raise HTTPException(409, detail={"error": "not_completed", "state": job.state})
    return FileResponse(_trace_path(job), media_type="application/x-ndjson")


@app.get("/api/runs/{run_id}/events")
async def stream_events(run_id: str, batch: int = Query(2000, ge=1, le=20000)) -> StreamingResponse:
    """Server-sent events: the trace in batches, then an ``end`` event.

    Waits while the run is queued or executing, so a client can subscribe as
    soon as it submits. Each ``trace`` message carries a JSON array of engine
    events, unmodified.
    """
    job = _job_or_404(runs, run_id)

    async def gen() -> Any:
        while job.state not in TERMINAL:
            yield f"event: state\ndata: {json.dumps({'state': job.state})}\n\n"
            await asyncio.sleep(0.25)
        if job.state != COMPLETED:
            yield f"event: end\ndata: {json.dumps({'state': job.state, 'error': job.error})}\n\n"
            return
        p = job.dir / "trace.jsonl"
        if not p.exists():
            yield 'event: end\ndata: {"state": "COMPLETED", "trace": false}\n\n'
            return
        buf: list[str] = []
        with p.open("r", encoding="utf-8") as f:
            for line in f:
                buf.append(line.rstrip("\n"))
                if len(buf) >= batch:
                    yield f"event: trace\ndata: [{','.join(buf)}]\n\n"
                    buf = []
                    await asyncio.sleep(0)
        if buf:
            yield f"event: trace\ndata: [{','.join(buf)}]\n\n"
        yield 'event: end\ndata: {"state": "COMPLETED", "trace": true}\n\n'

    return StreamingResponse(
        gen(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


# -----------------------------------------------------------------------------
# Comparisons
# -----------------------------------------------------------------------------


@app.post("/api/comparisons", status_code=202)
def create_comparison(req: ComparisonRequest) -> dict[str, Any]:
    sides = []
    for side in (req.a, req.b):
        cfg = side.model_dump(exclude={"label"})
        sides.append(
            RunRequest(
                **cfg,
                dataset_id=req.dataset_id,
                source_node=req.source_node,
                target_node=req.target_node,
                trace_level=req.trace_level,
                max_events=req.max_events,
                repetitions=req.repetitions,
                warmup=req.warmup,
            )
        )
    # Validate both before starting either: a comparison is all or nothing.
    info, _ = _ready_dataset(req.dataset_id)
    for s in sides:
        try:
            validate_against_dataset(s, info, registry)
        except ValueError as e:
            raise _bad(str(e)) from e

    def work(job: Job, m: JobManager) -> dict[str, Any]:
        m.set_state(job, RUNNING)
        ids = [_submit_run(s).id for s in sides]
        job.summary["runs"] = ids
        m.set_state(job, RUNNING, progress="both runs submitted")
        while True:
            states = [runs.get(i).state for i in ids]
            if all(s in TERMINAL for s in states):
                break
            if job.cancel_event.is_set():
                for i in ids:
                    runs.cancel(i)
            time.sleep(0.2)
        m.set_state(job, FINALIZING)
        if any(runs.get(i).state != COMPLETED for i in ids):
            bad = next(runs.get(i) for i in ids if runs.get(i).state != COMPLETED)
            raise JobFailure(
                "run_failed",
                f"run {bad.id} ended {bad.state}: {(bad.error or {}).get('message', '')}",
            )
        ra, rb = (_read_json(runs.get(i).dir / "result.json") for i in ids)
        same = (
            ra["dataset"]["checksum_fnv1a64"] == rb["dataset"]["checksum_fnv1a64"]
            and ra["request"]["source"] == rb["request"]["source"]
            and ra["request"]["target"] == rb["request"]["target"]
        )
        return {
            "runs": ids,
            "same_query": same,
            "reference_agrees": ra["reference"]["distance"] == rb["reference"]["distance"],
            "reference_distance": ra["reference"]["distance"],
        }

    job = comparisons.submit(
        req.model_dump(mode="json"),
        work,
        {"dataset_id": req.dataset_id, "labels": [req.a.label, req.b.label]},
    )
    return job.public()


def _benchmark_running() -> bool:
    return any(j.state in (RUNNING, FINALIZING) for j in benchmarks.list(5))


@app.post("/api/lane-comparisons", status_code=202)
def create_lane_comparison(req: LaneComparisonRequest) -> dict[str, Any]:
    """Time several (algorithm, queue) lanes on one query, fairly (see engine.py).

    Same graph, source and target for every lane; the same warm-up and repetition
    counts; tracing off; one lane at a time in a per-request shuffled order that is
    reported back; one admissible heuristic shared by every A* lane.
    """
    info, graph = _ready_dataset(req.dataset_id)
    n = int(info["nodes"])
    if req.source_node >= n or req.target_node >= n:
        raise _bad(f"source and target must be node ids below {n}")
    try:
        settings.tool("lpq_run")
    except FileNotFoundError as e:
        raise HTTPException(
            503, detail={"error": "engine_unavailable", "message": "lpq_run is not built"}
        ) from e
    try:
        _, run_reqs = lane_run_requests(req, info)
        for rr in run_reqs.values():
            validate_against_dataset(rr, info, registry)
    except ValueError as e:
        raise _bad(str(e)) from e

    def work(job: Job, m: JobManager) -> dict[str, Any]:
        return execute_lane_comparison(
            job, m, settings, registry, req, info, graph, _benchmark_running
        )

    job = lane_comparisons.submit(
        req.model_dump(mode="json"),
        work,
        {"dataset_id": req.dataset_id, "lanes": [lane.id for lane in req.lanes]},
    )
    return job.public()


@app.get("/api/lane-comparisons/{cmp_id}")
def get_lane_comparison(cmp_id: str) -> dict[str, Any]:
    """Job status; once COMPLETED, ``result`` holds every figure the page displays."""
    job = _job_or_404(lane_comparisons, cmp_id)
    out = job.public()
    out["request"] = _read_json(job.dir / "request.json")
    out["result"] = (
        _read_json(job.dir / "comparison.json")
        if job.state == COMPLETED and (job.dir / "comparison.json").exists()
        else None
    )
    return out


@app.post("/api/lane-comparisons/{cmp_id}/cancel")
def cancel_lane_comparison(cmp_id: str) -> dict[str, Any]:
    _job_or_404(lane_comparisons, cmp_id)
    return lane_comparisons.cancel(cmp_id).public()


@app.get("/api/comparisons")
def list_comparisons(limit: int = Query(30, ge=1, le=200)) -> list[dict[str, Any]]:
    return [j.public() for j in comparisons.list(limit)]


@app.get("/api/comparisons/{cmp_id}")
def get_comparison(cmp_id: str) -> dict[str, Any]:
    job = _job_or_404(comparisons, cmp_id)
    out = job.public()
    out["request"] = _read_json(job.dir / "request.json")
    return out


# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------


@app.post("/api/benchmarks", status_code=202)
def create_benchmark(req: BenchmarkRequest) -> dict[str, Any]:
    info, _ = _ready_dataset(req.dataset_id)
    if req.algorithm == "astar":
        cs = info.get("coordinate_system")
        if req.heuristic == "haversine" and cs != "geographic":
            raise _bad("the haversine heuristic needs geographic coordinates")
        if req.heuristic == "euclidean" and cs != "euclidean":
            raise _bad("the euclidean heuristic needs plane coordinates")
    if req.model_id:
        try:
            if registry.model(req.model_id).get("training_dataset") != req.dataset_id:
                raise _bad("the model was trained on a different dataset")
        except KeyError as e:
            raise _bad("unknown model") from e
    if req.query_manifest:
        try:
            registry.manifest_path(req.dataset_id, req.query_manifest)
        except KeyError as e:
            raise _bad("unknown query manifest for this dataset") from e

    def work(job: Job, m: JobManager) -> dict[str, Any]:
        return execute_benchmark(job, m, settings, registry, req, info)

    job = benchmarks.submit(
        req.model_dump(mode="json"),
        work,
        {"dataset_id": req.dataset_id, "queues": list(req.queues)},
    )
    return job.public()


@app.get("/api/benchmarks")
def list_benchmarks(limit: int = Query(30, ge=1, le=200)) -> list[dict[str, Any]]:
    return [j.public() for j in benchmarks.list(limit)]


@app.get("/api/benchmarks/{bench_id}")
def get_benchmark(bench_id: str) -> dict[str, Any]:
    job = _job_or_404(benchmarks, bench_id)
    out = job.public()
    out["request"] = _read_json(job.dir / "request.json")
    return out


@app.post("/api/benchmarks/{bench_id}/cancel")
def cancel_benchmark(bench_id: str) -> dict[str, Any]:
    _job_or_404(benchmarks, bench_id)
    return benchmarks.cancel(bench_id).public()


@app.get("/api/benchmarks/{bench_id}/results")
def get_benchmark_results(bench_id: str, raw: bool = False) -> Any:
    job = _job_or_404(benchmarks, bench_id)
    path = job.dir / "results.json"
    if job.state != COMPLETED or not path.exists():
        raise HTTPException(409, detail={"error": "no_results", "state": job.state})
    if raw:
        return FileResponse(
            path, media_type="application/json", filename=f"{bench_id}_results.json"
        )
    req = _read_json(job.dir / "request.json")
    return analysis.summarise_file(path, req.get("baseline"))


# -----------------------------------------------------------------------------
# Research artifacts
# -----------------------------------------------------------------------------


@app.get("/api/research/results")
def research_results() -> list[dict[str, Any]]:
    out = []
    for p in analysis.list_result_files(settings.root):
        rid = analysis.result_id(settings.root, p)
        group = rid.rpartition("/")[0]
        try:
            s = analysis.summarise_file(p, compare=False)
        except (ValueError, KeyError, TypeError, OSError):
            out.append({"file": rid, "group": group, "error": "malformed result file"})
            continue
        out.append(
            {
                "file": rid,
                "group": group,
                "experiment_id": s["experiment_id"],
                "generated_at": s["generated_at"],
                "dataset": s["dataset"],
                "dataset_nodes": s["graph_nodes"],
                "records": s["records"],
                "configurations": [c["key"] for c in s["configurations"]],
                "fastest_by_median": s["fastest_by_median"],
            }
        )
    return out


@app.get("/api/research/results/{file:path}")
def research_result(
    file: str, baseline: str | None = Query(None, pattern=r"^[a-z_0-9]+$")
) -> dict[str, Any]:
    p = analysis.find_result_file(settings.root, file)
    if p is None:
        raise HTTPException(404, detail={"error": "not_found", "message": "unknown result file"})
    try:
        s = analysis.summarise_file(p, baseline)
    except (ValueError, KeyError, TypeError, OSError):
        raise HTTPException(
            422, detail={"error": "malformed", "message": "result file could not be read"}
        ) from None
    s["file"] = file
    return s


@app.get("/api/research/notes")
def research_notes() -> list[dict[str, Any]]:
    notes = []
    for d in (settings.root / "docs" / "experiments", settings.root / "experiments" / "paper"):
        for p in sorted(d.glob("*.md")):
            text = p.read_text(encoding="utf-8")
            title = next(
                (ln.lstrip("# ").strip() for ln in text.splitlines() if ln.startswith("#")), p.stem
            )
            notes.append({"id": f"{d.name}/{p.stem}", "title": title, "markdown": text})
    return notes
