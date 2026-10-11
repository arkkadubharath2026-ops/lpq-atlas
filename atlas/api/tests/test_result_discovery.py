"""Stored benchmark results: discovery and loading (regression for the headline/ mismatch).

`scripts/bench/run_headline.sh` writes results/raw/headline/*.json and
`scripts/demo/reproduce_benchmark.{sh,ps1}` writes results/raw/local/*.json. The API used to
glob only results/raw/*.json, so neither script's output ever reached the Research or
Benchmarks pages. The unit tests below need no engine; the HTTP tests use the session
`client` fixture (and so need the built release binaries, like the rest of this suite).
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "atlas" / "api"))
sys.path.insert(0, str(REPO / "python"))

from atlas_api import analysis  # noqa: E402


def _record(pq: str, median_ns: float, source: int, target: int, dataset: str = "demo_road") -> dict:
    return {
        "algorithm": "dijkstra",
        "pq": pq,
        "mode": "exact",
        "dataset": dataset,
        "graph_nodes": 100,
        "source": source,
        "target": target,
        "supported": True,
        "timing": {"median_ns": median_ns, "relative_mad": 0.01, "cpu_pinned": True},
        "pq_operations": {"comparisons": 10, "pops": 5, "settled_nodes": 5, "relaxations": 9},
        "correctness": {"exact_match": True, "target_reachable": True},
        "environment": {"cpu": "test"},
    }


def _result(experiment_id: str, dataset: str = "demo_road") -> dict:
    recs = []
    for i, (s, t) in enumerate([(1, 2), (3, 4), (5, 6), (7, 8)]):
        recs.append(_record("binary_heap", 1000.0 + i, s, t, dataset))
        recs.append(_record("radix_heap", 900.0 + i, s, t, dataset))
    return {
        "experiment_id": experiment_id,
        "generated_at": "2026-01-01T00:00:00Z",
        "schema_version": 1,
        "records": recs,
    }


def _write(raw: Path, rel: str, payload: object) -> Path:
    p = raw / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(payload if isinstance(payload, str) else json.dumps(payload), encoding="utf-8")
    return p


def test_missing_results_directory_is_an_empty_listing(tmp_path: Path) -> None:
    assert analysis.list_result_files(tmp_path) == []
    assert analysis.find_result_file(tmp_path, "anything.json") is None


def test_nested_script_output_directories_are_discovered(tmp_path: Path) -> None:
    raw = tmp_path / "results" / "raw"
    _write(raw, "top.json", _result("top"))
    _write(raw, "headline/dimacs_ny_dijkstra.json", _result("h", "dimacs_ny"))
    _write(raw, "local/20260101T000000Z-demo_road.json", _result("l"))
    ids = [analysis.result_id(tmp_path, p) for p in analysis.list_result_files(tmp_path)]
    assert ids == [
        "headline/dimacs_ny_dijkstra.json",
        "local/20260101T000000Z-demo_road.json",
        "top.json",
    ]


def test_non_json_files_and_excess_depth_are_ignored(tmp_path: Path) -> None:
    raw = tmp_path / "results" / "raw"
    _write(raw, "notes.txt", "x")
    _write(raw, "a/b/c/d/e/deep.json", _result("deep"))
    _write(raw, "ok/fine.json", _result("fine"))
    ids = [analysis.result_id(tmp_path, p) for p in analysis.list_result_files(tmp_path)]
    assert ids == ["ok/fine.json"]


@pytest.mark.skipif(os.name == "nt", reason="symlinks need privileges on Windows")
def test_symlinks_cannot_expose_files_outside_results_raw(tmp_path: Path) -> None:
    raw = tmp_path / "results" / "raw"
    outside = tmp_path / "secret.json"
    outside.write_text(json.dumps(_result("secret")), encoding="utf-8")
    raw.mkdir(parents=True)
    (raw / "link.json").symlink_to(outside)
    assert analysis.list_result_files(tmp_path) == []


def test_lookup_is_by_listing_not_by_path_join(tmp_path: Path) -> None:
    raw = tmp_path / "results" / "raw"
    _write(raw, "headline/x.json", _result("x"))
    (tmp_path / "secret.json").write_text("{}", encoding="utf-8")
    assert analysis.find_result_file(tmp_path, "headline/x.json") is not None
    for bad in ("../../secret.json", "/etc/passwd", "headline/../../../secret.json", "headline"):
        assert analysis.find_result_file(tmp_path, bad) is None


def test_summary_identifies_dataset_and_configurations(tmp_path: Path) -> None:
    raw = tmp_path / "results" / "raw"
    p = _write(raw, "headline/x.json", _result("x", "dimacs_ny"))
    s = analysis.summarise_file(p)
    assert s["dataset"] == "dimacs_ny"
    assert {c["pq"] for c in s["configurations"]} == {"binary_heap", "radix_heap"}
    assert s["environment"] == {"cpu": "test"}
    assert s["fastest_by_median"] == "dijkstra/radix_heap/exact"


# ---- HTTP: the API as the web pages use it ------------------------------------------------


@pytest.fixture()
def raw_dir(fixture_root: Path):  # type: ignore[no-untyped-def]
    raw = fixture_root / "results" / "raw"
    before = set(raw.rglob("*")) if raw.exists() else set()
    yield raw
    for p in sorted(raw.rglob("*"), reverse=True):
        if p not in before:
            p.unlink() if p.is_file() or p.is_symlink() else p.rmdir()


def test_api_lists_and_loads_nested_results(client, raw_dir: Path) -> None:  # type: ignore[no-untyped-def]
    _write(raw_dir, "headline/dimacs_ny_dijkstra.json", _result("h", "dimacs_ny"))
    _write(raw_dir, "local/run.json", _result("l"))
    _write(raw_dir, "broken/bad.json", "{ not json")
    listing = {r["file"]: r for r in client.get("/api/research/results").json()}

    h = listing["headline/dimacs_ny_dijkstra.json"]
    assert h["group"] == "headline" and h["dataset"] == "dimacs_ny" and h["records"] == 8
    assert listing["local/run.json"]["group"] == "local"
    assert listing["broken/bad.json"]["error"] == "malformed result file"

    ok = client.get("/api/research/results/headline/dimacs_ny_dijkstra.json")
    assert ok.status_code == 200
    body = ok.json()
    assert body["file"] == "headline/dimacs_ny_dijkstra.json"
    assert body["dataset"] == "dimacs_ny"
    assert body["paired_comparisons"], "a binary_heap baseline exists, so a paired comparison is expected"

    # The web client percent-encodes each segment; the slash itself stays a slash.
    assert client.get("/api/research/results/headline/dimacs_ny_dijkstra.json?baseline=radix_heap").status_code == 200
    assert client.get("/api/research/results/broken/bad.json").status_code == 422


def test_api_unknown_and_traversal_ids_are_404(client, raw_dir: Path) -> None:  # type: ignore[no-untyped-def]
    _write(raw_dir, "headline/x.json", _result("x"))
    for bad in ("missing.json", "headline/missing.json", "..%2F..%2Fpyproject.toml", "headline/%2e%2e/%2e%2e/x.json"):
        r = client.get(f"/api/research/results/{bad}")
        assert r.status_code == 404, bad


def test_api_empty_results_directory_is_an_empty_list(client, fixture_root: Path) -> None:  # type: ignore[no-untyped-def]
    raw = fixture_root / "results" / "raw"
    if any(raw.rglob("*.json")):
        pytest.skip("another test left result files behind")
    assert client.get("/api/research/results").json() == []


def test_prepare_rejects_malformed_dataset_ids(client) -> None:  # type: ignore[no-untyped-def]
    for bad in ("Bad-Id", "x" * 65, "has.dot", "..", "a%20b"):
        r = client.post(f"/api/datasets/{bad}/prepare")
        assert r.status_code in (404, 422), bad
        assert r.status_code != 202, bad
