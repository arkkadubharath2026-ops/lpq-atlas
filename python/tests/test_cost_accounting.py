"""M6: what the benchmark's timed number contains, and where the other costs are recorded.

These tests pin the accounting that docs/experiments/m6-cost-accounting.md describes, on the small demo
graph. They are CHARACTERISATION tests: they say what the harness records today (including that it does not
yet record per-component ML time), not that the accounting is complete. They skip, with the command that
fixes it, when ``lpq_benchmark`` or the demo graph is not built.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from lpq_tools.data.queries import QueryManifest, read_manifest, write_manifest
from lpq_tools.training.model_io import (
    ModelKind,
    expand_static_weights,
    write_linear_model,
)
from lpq_tools.utilities.paths import RepoPaths

_PATHS = RepoPaths.discover()
_GRAPH = _PATHS.root / "datasets" / "processed" / "demo_road.lpqgraph"
_QUERIES = _PATHS.root / "experiments" / "manifests" / "demo_road_queries.yaml"
_COUNT = 6
_REPS = 3


@pytest.fixture(scope="module")
def tool() -> str:
    try:
        path = str(_PATHS.tool("lpq_benchmark"))
    except FileNotFoundError as error:
        pytest.skip(str(error))
    if not _GRAPH.exists():
        pytest.skip("demo graph not prepared: python scripts/demo/prepare_demo.py")
    return path


@pytest.fixture(scope="module")
def queries(tmp_path_factory) -> Path:
    full = read_manifest(_QUERIES)
    prefix = QueryManifest(full.dataset_id, full.node_count, full.seed, full.pairs[:_COUNT],
                           full.graph_checksum)
    path = tmp_path_factory.mktemp("accounting") / "queries.yaml"
    write_manifest(path, prefix)
    return path


@pytest.fixture
def model(tmp_path: Path) -> Path:
    path = tmp_path / "m.lpqmodel"
    write_linear_model(path, expand_static_weights([1.5, -0.4, 0.8, -0.2, 0.3, 0.6]), -1.0,
                       kind=ModelKind.LOGISTIC)
    return path


def _run(tool: str, graph: Path, queries: Path, out: Path, *args: str) -> list[dict]:
    result = subprocess.run(
        [tool, "--dataset", str(graph), "--queries-file", str(queries), "--repetitions", str(_REPS),
         "--warmup", "1", "--lpq-bucket-width", "20", "--output", str(out), *args],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(out.read_text())["records"]


def test_the_online_update_cost_is_per_query_and_not_among_the_timed_samples(tool, queries, model,
                                                                              tmp_path):
    records = _run(tool, _GRAPH, queries, tmp_path / "online.json", "--pq", "lpq_online",
                   "--evaluation", "online_session", "--online-update-mode", "periodic",
                   "--model", str(model), "--online-model-no-provenance")
    assert len(records) == _COUNT
    for r in records:
        # One entry per measured repetition: the update, applied once after the last one, is not one of them.
        assert len(r["timing"]["samples_ns"]) == r["timing"]["repetitions"] == _REPS
        assert r["ml"]["online_update_ns"] > 0
        assert "learner" in r
    # The update cost is a per-query figure: it is not a function of how many repetitions were timed.
    assert all(r["ml"]["total_overhead_ns"] == r["ml"]["online_update_ns"] for r in records)


def test_a_static_row_records_no_update_cost_and_no_learner(tool, queries, model, tmp_path):
    records = _run(tool, _GRAPH, queries, tmp_path / "static.json", "--pq", "lpq_model",
                   "--model", str(model))
    for r in records:
        assert r["ml"]["online_update_ns"] == 0
        assert "learner" not in r
        assert len(r["timing"]["samples_ns"]) == _REPS


def test_the_harness_does_not_yet_record_per_component_ml_time(tool, queries, model, tmp_path):
    """Pins a known gap (D-26): feature_extraction_ns, inference_ns and verification_ns are always 0 here.

    If they are ever populated this test must change on purpose, and the M6 document with it.
    """
    records = _run(tool, _GRAPH, queries, tmp_path / "gap.json", "--pq", "lpq_model",
                   "--model", str(model))
    for r in records:
        assert (r["ml"]["feature_extraction_ns"], r["ml"]["inference_ns"], r["ml"]["verification_ns"]) \
            == (0, 0, 0)


def test_lpq_optimized_reports_its_one_time_costs_beside_the_timed_samples(tool, queries, tmp_path):
    # The tuning decision is cached next to the graph, so work on a private copy of it.
    graph = tmp_path / "demo_road.lpqgraph"
    shutil.copy(_GRAPH, graph)
    first = _run(tool, graph, queries, tmp_path / "opt1.json", "--compare", "radix_heap;lpq_optimized")
    second = _run(tool, graph, queries, tmp_path / "opt2.json", "--compare", "radix_heap;lpq_optimized")

    def optimized(records):
        return [r for r in records if r["pq"] == "lpq_optimized"]

    for r in optimized(first) + optimized(second):
        qc = r["queue_config"]
        assert {"resolved_queue", "resolution_method", "calibration_ns", "profile_ns"} <= set(qc)
        assert len(r["timing"]["samples_ns"]) == _REPS  # calibration is not among the samples
    assert optimized(first)[0]["queue_config"]["resolution_method"] == "autotune"
    assert optimized(first)[0]["queue_config"]["calibration_ns"] > 0
    # The decision is cached: the second run pays no calibration, and says so.
    assert optimized(second)[0]["queue_config"]["resolution_method"] == "autotune_cached"
    assert optimized(second)[0]["queue_config"]["calibration_ns"] == 0
