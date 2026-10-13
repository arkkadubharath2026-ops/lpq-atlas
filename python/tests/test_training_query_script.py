"""Tests for scripts/data/generate_training_queries.py (source-disjoint training manifests)."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest
from lpq_tools.data.queries import build_manifest, read_manifest, write_manifest
from lpq_tools.utilities.hashing import sha256_file

_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "data" / "generate_training_queries.py"


def _load():
    spec = importlib.util.spec_from_file_location("generate_training_queries", _SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def benchmark(tmp_path):
    path = tmp_path / "toy_queries.yaml"
    write_manifest(path, build_manifest("toy", 400, count=120, seed=42, graph_checksum=77))
    return path


def test_the_output_shares_no_source_with_the_benchmark(tmp_path, benchmark, capsys):
    out = tmp_path / "toy_training_queries_source_disjoint.yaml"
    code = _load().main(
        ["--benchmark", str(benchmark), "--seed", "917", "--count", "60", "--output", str(out)]
    )
    assert code == 0
    train, bench = read_manifest(out), read_manifest(benchmark)
    assert {p.source for p in train.pairs}.isdisjoint({p.source for p in bench.pairs})
    assert len(train.pairs) == 60
    assert (train.dataset_id, train.node_count, train.graph_checksum) == ("toy", 400, 77)
    assert train.selection == "uniform_pairs_distinct_endpoints_source_disjoint"
    text = out.read_text()
    assert sha256_file(benchmark) in text, "the manifest records which benchmark file it excludes"
    printed = capsys.readouterr().out
    assert "allowed sources" in printed


def test_every_extra_manifest_is_excluded_too(tmp_path, benchmark):
    extra = tmp_path / "toy_other.yaml"
    write_manifest(extra, build_manifest("toy", 400, count=120, seed=5, graph_checksum=77))
    out = tmp_path / "t.yaml"
    assert _load().main(
        ["--benchmark", str(benchmark), "--also-exclude", str(extra),
         "--seed", "917", "--count", "50", "--output", str(out)]
    ) == 0
    sources = {p.source for p in read_manifest(out).pairs}
    assert sources.isdisjoint({p.source for p in read_manifest(extra).pairs})
    assert sources.isdisjoint({p.source for p in read_manifest(benchmark).pairs})


def test_it_refuses_to_overwrite(tmp_path, benchmark, capsys):
    out = tmp_path / "t.yaml"
    out.write_text("keep me\n")
    code = _load().main(
        ["--benchmark", str(benchmark), "--seed", "1", "--count", "5", "--output", str(out)]
    )
    assert code == 1
    assert out.read_text() == "keep me\n"
    assert "refusing to overwrite" in capsys.readouterr().err


def test_a_manifest_for_a_different_graph_is_refused(tmp_path, benchmark, capsys):
    other = tmp_path / "other.yaml"
    write_manifest(other, build_manifest("toy", 400, count=10, seed=3, graph_checksum=78))
    code = _load().main(
        ["--benchmark", str(benchmark), "--also-exclude", str(other),
         "--seed", "1", "--count", "5", "--output", str(tmp_path / "t.yaml")]
    )
    assert code == 1
    assert "different graph" in capsys.readouterr().err
    assert not (tmp_path / "t.yaml").exists()


def test_a_benchmark_that_uses_every_node_leaves_nothing_to_draw(tmp_path, capsys):
    full = tmp_path / "full.yaml"
    manifest = build_manifest("toy", 6, count=400, seed=2, graph_checksum=1)
    assert len({p.source for p in manifest.pairs}) == 6
    write_manifest(full, manifest)
    code = _load().main(
        ["--benchmark", str(full), "--seed", "1", "--count", "5", "--output", str(tmp_path / "t.yaml")]
    )
    assert code == 1
    assert "no source" in capsys.readouterr().err
