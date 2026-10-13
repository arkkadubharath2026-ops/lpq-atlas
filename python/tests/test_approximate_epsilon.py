"""D-17: an approximate-mode result file states the epsilon it was run against.

The runner always enforced the bound while running (it aborts on ``ratio > 1 + epsilon``), but the
stored records did not say which epsilon that was, so a file could not be re-checked later. These
tests run the real ``lpq_benchmark`` and read the JSON it wrote, through both entry points: the
``--epsilon`` option and an experiment configuration (``--experiment``), which is the path the
reproduction scripts use. They skip, with the command that fixes it, when the binary or the demo graph
is not built.
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


# --- the experiment-configuration path ------------------------------------------------------------


def _config(path: Path, name: str, queries: Path, modes: list[str], pqs: list[str],
            epsilon: str) -> Path:
    """An experiment configuration like ``configs/experiments/dimacs_ny_eps_010_v1.yaml``, small enough
    for a test: the demo graph, an explicit query file and the minimum repetitions a manifest allows."""
    modes_yaml = "".join(f"  - {m}\n" for m in modes)
    pqs_yaml = "".join(f"  - {p}\n" for p in pqs)
    path.write_text(
        f"experiment: {name}\n"
        "dataset: demo_road\n"
        f"queries_file: {queries}\n"
        "algorithm:\n  - dijkstra\n"
        f"pq:\n{pqs_yaml}"
        f"mode:\n{modes_yaml}"
        f"epsilon: {epsilon}\n"
        f"query_count: {_COUNT}\n"
        "repetitions: 3\nwarmup: 1\nseed: 42\n"
        "collect:\n  runtime: true\n  pq_operations: true\n  ml_latency: false\n"
        "  hardware_counters: true\n",
        encoding="utf-8",
    )
    return path


def _run_config(tool: str, config: Path, tmp_path: Path, name: str) -> list[dict]:
    out = tmp_path / f"{name}.json"
    # --experiment resolves datasets/processed/<dataset>.lpqgraph against the working directory.
    result = subprocess.run([tool, "--experiment", str(config), "--output", str(out)],
                            capture_output=True, text=True, check=False, cwd=_PATHS.root)
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(out.read_text())["records"]


@pytest.mark.parametrize("epsilon", ["0.1", "0.35"])
def test_the_epsilon_in_an_experiment_configuration_reaches_every_approximate_record(
        tool, queries, tmp_path, epsilon):
    config = _config(tmp_path / "cfg.yaml", f"eps_cfg_{epsilon}", queries, ["approximate"],
                     ["binary_heap", "lpq"], epsilon)
    records = _run_config(tool, config, tmp_path, "cfg")

    measured = [r for r in records if r["pq"] == "lpq"]
    unsupported = [r for r in records if r["pq"] == "binary_heap"]
    assert len(measured) == len(unsupported) == _COUNT
    for r in measured:
        assert r["supported"] and r["mode"] == "approximate"
        assert r["experiment_id"] == f"eps_cfg_{epsilon}"
        assert r["correctness"]["epsilon"] == float(epsilon)
        assert r["correctness"]["approximation_ratio"] <= 1.0 + float(epsilon)
    for r in unsupported:  # the classical heap has no approximation mechanism: a stated non-result
        assert not r["supported"]
        assert r["correctness"]["epsilon"] == float(epsilon)


def test_a_configuration_mixing_exact_and_approximate_modes_marks_only_the_approximate_rows(
        tool, queries, tmp_path):
    config = _config(tmp_path / "mixed.yaml", "eps_mixed", queries, ["exact", "approximate"],
                     ["lpq"], "0.1")
    records = _run_config(tool, config, tmp_path, "mixed")

    by_mode = {m: [r for r in records if r["mode"] == m] for m in ("exact", "approximate")}
    assert len(by_mode["exact"]) == len(by_mode["approximate"]) == _COUNT
    assert all("epsilon" not in r["correctness"] for r in by_mode["exact"])
    assert all(r["correctness"]["epsilon"] == 0.1 for r in by_mode["approximate"])
