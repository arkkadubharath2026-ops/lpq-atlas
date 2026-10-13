"""The model report records which training manifest produced the model (D-21)."""

from __future__ import annotations

import dataclasses
import json
from pathlib import Path

import pytest
from lpq_tools.data.queries import (
    SELECTION_SOURCE_DISJOINT,
    build_manifest,
    write_manifest,
)
from lpq_tools.training import train
from lpq_tools.training.train import describe_training_manifest, train_from_file
from lpq_tools.utilities.hashing import sha256_file
from test_train import make_set


def _data(**overrides):
    base = {"dataset_id": "toy", "graph_checksum": 77, "query_count": 40, "query_seed": 917}
    return dataclasses.replace(make_set(), **{**base, **overrides})


def _manifest(tmp_path: Path, name: str = "toy_training_queries_source_disjoint.yaml", **kw) -> Path:
    args = {"count": 40, "seed": 917, "graph_checksum": 77, "exclude_sources": {1, 2, 3}}
    args.update(kw)
    path = tmp_path / name
    write_manifest(path, build_manifest("toy", 300, **args))
    return path


def test_the_record_names_hashes_and_describes_the_manifest(tmp_path):
    path = _manifest(tmp_path)
    record = describe_training_manifest(path, _data())
    assert record == {
        "name": "toy_training_queries_source_disjoint.yaml",
        "sha256": sha256_file(path),
        "selection": SELECTION_SOURCE_DISJOINT,
        "seed": 917,
        "query_count": 40,
    }


@pytest.mark.parametrize(
    ("overrides", "fragment"),
    [
        ({"dataset_id": "other"}, "dataset"),
        ({"graph_checksum": 78}, "graph"),
        ({"query_seed": 5}, "seed"),
        ({"query_count": 41}, "queries"),
    ],
)
def test_a_manifest_that_does_not_match_the_training_set_is_refused(tmp_path, overrides, fragment):
    # Recording the wrong manifest would be worse than recording none.
    with pytest.raises(ValueError, match=fragment):
        describe_training_manifest(_manifest(tmp_path), _data(**overrides))


def test_train_from_file_writes_the_record_into_the_report(tmp_path, monkeypatch):
    monkeypatch.setattr(train, "read_training_set_file", lambda _p: _data())
    path = _manifest(tmp_path)
    train_from_file(
        tmp_path / "x.lpqtrain", tmp_path / "m.lpqmodel", tmp_path / "m.report.json",
        seed=1, training_manifest=path,
    )
    report = json.loads((tmp_path / "m.report.json").read_text())
    assert report["training_manifest"]["name"] == path.name
    assert report["training_manifest"]["sha256"] == sha256_file(path)


def test_without_a_manifest_the_report_says_none_was_recorded(tmp_path, monkeypatch):
    monkeypatch.setattr(train, "read_training_set_file", lambda _p: _data())
    train_from_file(tmp_path / "x.lpqtrain", tmp_path / "m.lpqmodel", tmp_path / "m.report.json", seed=1)
    assert json.loads((tmp_path / "m.report.json").read_text())["training_manifest"] is None
