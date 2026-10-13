"""D-17: an approximate-mode result file states the epsilon it was run against.

The runner always enforced the bound while running (it aborts on ``ratio > 1 + epsilon``), but the
stored records did not say which epsilon that was, so a file could not be re-checked later. These
tests run the real ``lpq_benchmark`` and read the JSON it wrote. They skip, with the command that
fixes it, when the binary or the demo graph is not built.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from lpq_tools.data.queries import QueryManifest, read_manifest, write_manifest
from lpq_tools.utilities.paths import RepoPaths

_PATHS = RepoPaths.discover()
_GRAPH = _PATHS.root / "datasets" / "processed" / "demo_road.lpqgraph"
_QUERIES = _PATHS.root / "experiments" / "manifests" / "demo_road_queries.yaml"
_COUNT = 8


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
    path = tmp_path_factory.mktemp("demo") / "demo_road_queries_prefix.yaml"
    write_manifest(path, prefix)
    return path


def _run(tool: str, queries: Path, tmp_path: Path, name: str, *args: str) -> list[dict]:
    out = tmp_path / f"{name}.json"
    result = subprocess.run(
        [tool, "--dataset", str(_GRAPH), "--queries-file", str(queries), "--warmup", "0",
         "--repetitions", "1", "--lpq-bucket-width", "20", "--output", str(out), *args],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(out.read_text())["records"]


def test_every_approximate_record_carries_the_configured_epsilon(tool, queries, tmp_path):
    records = _run(tool, queries, tmp_path, "approx", "--pq", "lpq", "--mode", "approximate",
                   "--epsilon", "0.1")
    assert len(records) == _COUNT
    for r in records:
        assert r["mode"] == "approximate"
        assert r["correctness"]["epsilon"] == 0.1
        # The recorded bound is the one the ratio was checked against.
        assert r["correctness"]["approximation_ratio"] <= 1.0 + r["correctness"]["epsilon"]


def test_the_epsilon_is_the_one_configured_not_a_default(tool, queries, tmp_path):
    seen = {
        eps: {r["correctness"]["epsilon"]
              for r in _run(tool, queries, tmp_path, f"eps{eps}", "--pq", "lpq", "--mode",
                            "approximate", "--epsilon", eps)}
        for eps in ("0.05", "0.5")
    }
    assert seen == {"0.05": {0.05}, "0.5": {0.5}}


def test_a_configuration_without_an_approximation_mechanism_still_states_its_epsilon(
        tool, queries, tmp_path):
    records = _run(tool, queries, tmp_path, "unsupported", "--pq", "binary_heap", "--mode",
                   "approximate", "--epsilon", "0.1")
    assert records
    for r in records:
        assert not r["supported"]
        assert r["correctness"]["epsilon"] == 0.1


def test_exact_records_have_no_epsilon_key(tool, queries, tmp_path):
    records = _run(tool, queries, tmp_path, "exact", "--pq", "lpq")
    assert len(records) == _COUNT
    for r in records:
        assert r["mode"] == "exact"
        assert "epsilon" not in r["correctness"]
