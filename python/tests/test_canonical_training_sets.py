"""Which training query sets the reproduction pipeline uses, and that they are source-disjoint.

Protocol C (owner decision, M5-P1): a training set shares no SOURCE node with any benchmark manifest
the pipeline measures, because lpq_collect labels from a full single-source search per source. The
earlier protocols are kept as historical evidence, and a test pins that they are unchanged.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from lpq_tools.data.queries import (
    build_manifest,
    read_manifest,
    shared_sources,
    write_manifest,
)
from lpq_tools.utilities.hashing import sha256_file

_ROOT = Path(__file__).resolve().parents[2]
_MANIFESTS = _ROOT / "experiments" / "manifests"
_COMMON = _ROOT / "scripts" / "reproduce" / "common.sh"

_FACEBOOK_BENCHMARKS = (
    "snap_facebook_queries.yaml",
    "snap_facebook_queries_first100.yaml",
    "snap_facebook_queries_first30.yaml",
)
_NEW_YORK_BENCHMARKS = ("dimacs_ny_queries_100.yaml", "dimacs_ny_queries_first30.yaml")

# Files that must stay exactly as committed: the benchmark manifests, and the protocol B training set
# that the historical measurements were made with.
_PINNED = {
    "snap_facebook_queries.yaml": "c495353a76f4e5abd4a1f0e1c0be18aa960022f8a5df839ba33c8716fb643b49",
    "snap_facebook_queries_first100.yaml": "a08a2696e6484875192ab8ebcbe54aa09746b5acb3a8a1c5f8b154abb52a98b3",
    "snap_facebook_queries_first30.yaml": "507acf45ec8500c2af3f41f55bb41587eb970ecb10d09d7295539af899b3c633",
    "snap_facebook_training_queries.yaml": "c9fbc237cfdfb2bbf1c7f55ca78beddb42211fe1404396846f54da7a207ab630",
    "dimacs_ny_training_queries.yaml": "65ab749cdb10c62a6b24a9b2eb0e0e355e3efc9819b788927dcfc1bd96989605",
    "dimacs_ny_queries_100.yaml": "b2bd5890d000fe15e7875c142a5459ab0d72870b878e327b0ecaafb100bbc89d",
    "dimacs_ny_queries_first30.yaml": "4057dd2e1ab0be6a23d232154b20237f76090edfc2ffd6356b3c44c22bb05456",
}


def _sources(name: str) -> set[int]:
    return {p.source for p in read_manifest(_MANIFESTS / name).pairs}


def test_the_facebook_protocol_c_training_set_shares_no_source_with_any_benchmark_manifest():
    training = read_manifest(_MANIFESTS / "snap_facebook_training_queries_source_disjoint.yaml")
    assert len(training.pairs) == 200
    for name in _FACEBOOK_BENCHMARKS:
        assert shared_sources(training, read_manifest(_MANIFESTS / name)) == set(), name


def test_the_new_york_training_set_shares_no_source_with_the_manifests_the_pipeline_uses():
    training = read_manifest(_MANIFESTS / "dimacs_ny_training_queries.yaml")
    for name in _NEW_YORK_BENCHMARKS:
        assert shared_sources(training, read_manifest(_MANIFESTS / name)) == set(), name


def test_protocol_b_stays_as_historical_evidence_and_is_still_not_source_disjoint():
    # The pair-disjoint Facebook set is what the earlier measurements used. It must not be edited to
    # look better, and the fact that it shares sources is the reason protocol C exists.
    b = read_manifest(_MANIFESTS / "snap_facebook_training_queries.yaml")
    main = read_manifest(_MANIFESTS / "snap_facebook_queries.yaml")
    assert len(shared_sources(b, main)) == 45


@pytest.mark.parametrize("name", sorted(_PINNED))
def test_benchmark_manifests_and_the_historical_training_sets_are_unchanged(name):
    assert sha256_file(_MANIFESTS / name) == _PINNED[name]


def test_shared_sources_looks_at_sources_only():
    a = build_manifest("g", 50, count=30, seed=1)
    b = build_manifest("g", 50, count=30, seed=2)
    expected = {p.source for p in a.pairs} & {p.source for p in b.pairs}
    assert shared_sources(a, b) == expected
    assert expected, "the fixture should overlap, otherwise this proves nothing"


# -----------------------------------------------------------------------------
# The shell selection used by scripts/reproduce/run_main_experiments.sh
# -----------------------------------------------------------------------------

pytestmark_bash = pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")


def _select(dataset: str, directory: Path | None = None) -> subprocess.CompletedProcess[str]:
    args = f'"{dataset}"' + (f' "{directory.as_posix()}"' if directory else "")
    env = {**os.environ, "LPQ_PYTHON": sys.executable}
    return subprocess.run(
        ["bash", "-c", f'source "{_COMMON.as_posix()}"; select_training_manifest {args}'],
        capture_output=True, text=True, env=env, check=False,
    )


@pytestmark_bash
def test_the_pipeline_trains_facebook_on_the_source_disjoint_set():
    result = _select("snap_facebook")
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith("snap_facebook_training_queries_source_disjoint.yaml")
    assert "source-disjoint" in result.stderr


@pytestmark_bash
def test_the_pipeline_trains_new_york_on_its_existing_set_after_verifying_it():
    result = _select("dimacs_ny")
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith("dimacs_ny_training_queries.yaml")


def _write(directory: Path, name: str, **kwargs) -> None:
    kwargs.setdefault("graph_checksum", 9)
    write_manifest(directory / name, build_manifest("dimacs_ny", 500, **kwargs))


@pytestmark_bash
def test_a_training_set_that_shares_a_source_is_refused(tmp_path):
    for name in ("dimacs_ny_queries_100.yaml", "dimacs_ny_queries_first30.yaml"):
        _write(tmp_path, name, count=60, seed=42)
    _write(tmp_path, "dimacs_ny_training_queries.yaml", count=60, seed=42)  # identical sources
    result = _select("dimacs_ny", tmp_path)
    assert result.returncode != 0
    assert "share" in result.stderr and "source" in result.stderr
    assert result.stdout == ""


@pytestmark_bash
def test_a_source_disjoint_set_is_preferred_when_it_exists(tmp_path):
    bench = build_manifest("dimacs_ny", 500, count=60, seed=42, graph_checksum=9)
    for name in ("dimacs_ny_queries_100.yaml", "dimacs_ny_queries_first30.yaml"):
        write_manifest(tmp_path / name, bench)
    _write(tmp_path, "dimacs_ny_training_queries.yaml", count=60, seed=42)  # overlapping, would fail
    _write(
        tmp_path, "dimacs_ny_training_queries_source_disjoint.yaml", count=40, seed=917,
        exclude_sources={p.source for p in bench.pairs},
    )
    result = _select("dimacs_ny", tmp_path)
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith("dimacs_ny_training_queries_source_disjoint.yaml")


@pytestmark_bash
def test_a_manifest_for_another_graph_is_refused(tmp_path):
    for name in ("dimacs_ny_queries_100.yaml", "dimacs_ny_queries_first30.yaml"):
        _write(tmp_path, name, count=30, seed=42)
    _write(tmp_path, "dimacs_ny_training_queries.yaml", count=30, seed=917, graph_checksum=10)
    assert _select("dimacs_ny", tmp_path).returncode != 0
