"""The model page identifies the training manifest from the report, not from a file name (D-21)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from atlas_api import registry  # puts the research tooling (python/) on sys.path
from lpq_tools.data.queries import (
    SELECTION_SOURCE_DISJOINT,
    QueryPair,
    build_manifest,
    write_manifest,
)
from lpq_tools.utilities.hashing import sha256_file

DATASET = "toy"


def _write(root: Path, name: str, manifest) -> Path:
    path = root / "experiments" / "manifests" / name
    write_manifest(path, manifest)
    return path


def _manifest(pairs, selection=None, count=None):
    base = build_manifest(DATASET, 100, count=1, seed=1, graph_checksum=5)
    fields = {
        "dataset_id": DATASET, "node_count": 100, "seed": 917, "graph_checksum": 5,
        "pairs": tuple(QueryPair(s, t) for s, t in pairs),
    }
    if selection:
        fields["selection"] = selection
    return type(base)(**fields)


def _model(root: Path, report: dict) -> Path:
    model = root / "models" / "exported" / "m_static.lpqmodel"
    model.parent.mkdir(parents=True, exist_ok=True)
    model.write_bytes(b"not a real model")
    model.with_name("m_static.report.json").write_text(json.dumps(report), encoding="utf-8")
    return model


def _info(model: Path) -> dict:
    # __wrapped__ bypasses the lru_cache; the nonexistent tool makes the loader step fail cleanly.
    return registry._model_info.__wrapped__(str(model), model.stat().st_mtime, "/nonexistent/lpq_model_info")


def _record(path: Path, **kw) -> dict:
    return {
        "name": path.name, "sha256": sha256_file(path), "selection": SELECTION_SOURCE_DISJOINT,
        "seed": 917, "query_count": 1, **kw,
    }


def test_the_recorded_manifest_is_used_even_when_a_differently_named_one_exists(tmp_path):
    # The dataset-named file overlaps the benchmark by pair, so inferring it would say OVERLAPPING.
    _write(tmp_path, f"{DATASET}_training_queries.yaml", _manifest([(1, 2)]))
    recorded = _write(tmp_path, f"{DATASET}_training_queries_source_disjoint.yaml",
                      _manifest([(7, 8)], SELECTION_SOURCE_DISJOINT))
    _write(tmp_path, f"{DATASET}_queries.yaml", _manifest([(1, 2)]))
    info = _info(_model(tmp_path, {"dataset_id": DATASET, "training_manifest": _record(recorded)}))
    assert info["training_manifest"] == recorded.name
    assert info["training_manifest_provenance"]["status"] == "verified"
    assert info["training_manifest_provenance"]["source"] == "recorded"
    assert info["split_status"] == "DISJOINT"
    assert info["splits"][0]["level"] == "source"


def test_a_source_overlap_with_a_declared_source_disjoint_manifest_is_flagged(tmp_path):
    recorded = _write(tmp_path, f"{DATASET}_training_queries_source_disjoint.yaml",
                      _manifest([(1, 2)], SELECTION_SOURCE_DISJOINT))
    _write(tmp_path, f"{DATASET}_queries.yaml", _manifest([(1, 9)]))
    info = _info(_model(tmp_path, {"dataset_id": DATASET, "training_manifest": _record(recorded)}))
    assert info["split_status"] == "OVERLAPPING"
    assert info["splits"][0]["shared_sources"] == 1


def test_a_manifest_that_changed_since_training_is_not_trusted(tmp_path):
    recorded = _write(tmp_path, f"{DATASET}_training_queries_source_disjoint.yaml",
                      _manifest([(7, 8)], SELECTION_SOURCE_DISJOINT))
    _write(tmp_path, f"{DATASET}_queries.yaml", _manifest([(1, 2)]))
    record = _record(recorded, sha256="0" * 64)
    info = _info(_model(tmp_path, {"dataset_id": DATASET, "training_manifest": record}))
    assert info["training_manifest_provenance"]["status"] == "hash_mismatch"
    assert info["split_status"] == "UNKNOWN"
    assert info["splits"] == []


def test_a_recorded_manifest_that_is_missing_is_reported_not_replaced(tmp_path):
    _write(tmp_path, f"{DATASET}_training_queries.yaml", _manifest([(7, 8)]))
    _write(tmp_path, f"{DATASET}_queries.yaml", _manifest([(1, 2)]))
    record = {"name": "gone.yaml", "sha256": "0" * 64, "selection": "x", "seed": 1, "query_count": 1}
    info = _info(_model(tmp_path, {"dataset_id": DATASET, "training_manifest": record}))
    assert info["training_manifest_provenance"]["status"] == "manifest_missing"
    assert info["split_status"] == "UNKNOWN"


@pytest.mark.parametrize("report_extra", [{}, {"training_manifest": None}])
def test_an_older_report_falls_back_to_the_legacy_inference_and_says_so(tmp_path, report_extra):
    legacy = _write(tmp_path, f"{DATASET}_training_queries.yaml", _manifest([(7, 8)]))
    _write(tmp_path, f"{DATASET}_queries.yaml", _manifest([(1, 2)]))
    info = _info(_model(tmp_path, {"dataset_id": DATASET, **report_extra}))
    prov = info["training_manifest_provenance"]
    assert info["training_manifest"] == legacy.name
    assert (prov["recorded"], prov["status"], prov["source"]) == (
        False, "not_recorded", "inferred_from_dataset_filename",
    )
    assert info["split_status"] == "DISJOINT"
    assert info["splits"][0]["level"] == "pair"


def test_a_report_with_no_dataset_and_no_record_has_no_manifest(tmp_path):
    info = _info(_model(tmp_path, {}))
    assert info["training_manifest_provenance"]["status"] == "not_recorded"
    assert info["split_status"] == "UNKNOWN"
