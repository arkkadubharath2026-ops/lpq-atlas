"""lpq_run with the online learner, through the real engine (M5-O1, O4).

C++ unit tests prove frozen equivalence on random graphs. These run the real ``lpq_run`` on a real
converted graph and check the same claim at the command line, plus what the result says about the
learner: attachment, recorded events, applied updates and a measured weight change are separate facts
(D-12), and a model is refused or its override recorded before any query runs.

They skip, with the command that fixes it, when ``lpq_run`` or the demo graph has not been built.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest
from lpq_tools.data.queries import read_manifest
from lpq_tools.training.model_io import (
    ModelKind,
    expand_static_weights,
    write_linear_model,
)
from lpq_tools.utilities.hashing import sha256_file
from lpq_tools.utilities.paths import RepoPaths

_PATHS = RepoPaths.discover()
_GRAPH = _PATHS.root / "datasets" / "processed" / "demo_road.lpqgraph"
_META = _PATHS.root / "datasets" / "metadata" / "demo_road.json"
_QUERIES = _PATHS.root / "experiments" / "manifests" / "demo_road_queries.yaml"


@pytest.fixture(scope="module")
def run_tool() -> str:
    try:
        tool = str(_PATHS.tool("lpq_run"))
    except FileNotFoundError as error:
        pytest.skip(str(error))
    if not _GRAPH.exists():
        pytest.skip("demo graph not prepared: python scripts/demo/prepare_demo.py")
    return tool


def _run(tool: str, query: tuple[int, int], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [tool, "--dataset", str(_GRAPH), "--source", str(query[0]), "--target", str(query[1]),
         "--repetitions", "0", *args],
        capture_output=True, text=True, check=False,
    )


def _ok(tool: str, query: tuple[int, int], *args: str) -> dict:
    result = _run(tool, query, *args)
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(result.stdout)


def _queries(count: int) -> list[tuple[int, int]]:
    return [(p.source, p.target) for p in read_manifest(_QUERIES).pairs[:count]]


def _model(tmp_path: Path, *, weights=(1.5, -0.4, 0.8, -0.2, 0.3, 0.6), bias=-1.0, name="m") -> Path:
    path = tmp_path / f"{name}.lpqmodel"
    write_linear_model(path, expand_static_weights(list(weights)), bias, kind=ModelKind.LOGISTIC)
    return path


def _report(tmp_path: Path, **overrides) -> Path:
    meta = json.loads(_META.read_text(encoding="utf-8"))
    report = {"dataset_id": "demo_road", "graph_checksum": meta["graph_checksum_fnv1a64"],
              "bucket_width": 20, "query_count": 10, "query_seed": 1, "seed": 1}
    report.update(overrides)
    path = tmp_path / "m.report.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return path


def _search_facts(doc: dict) -> dict:
    """Everything about the search itself, none of the timing or the learner block."""
    lpq = {k: v for k, v in doc["lpq"].items()
           if k in ("metrics", "candidate", "verification", "correction", "bucket_width", "window")}
    return {"result": doc["result"], "counters": doc["counters"], "lpq": lpq}


# -----------------------------------------------------------------------------
# Frozen equivalence at the command line
# -----------------------------------------------------------------------------


@pytest.mark.parametrize("update_mode", ["disabled", "periodic", "every_event", "adaptive"])
def test_lpq_online_searches_exactly_like_lpq_model_with_the_same_artifact(
    run_tool, tmp_path, update_mode
):
    model = _model(tmp_path)
    common = ["--lpq-bucket-width", "20"]
    online_flags = ["--online-model", str(model), "--online-model-no-provenance",
                    "--online-update-mode", update_mode]
    for query in _queries(12):
        frozen = _ok(run_tool, query, "--pq", "lpq_model", "--model", str(model), *common)
        online = _ok(run_tool, query, "--pq", "lpq_online", *online_flags, *common)
        assert _search_facts(online) == _search_facts(frozen), (update_mode, query)
        assert online["correctness"]["exact_match"] is True


def test_the_learner_block_separates_what_happened_and_never_claims_improvement(run_tool, tmp_path):
    model = _model(tmp_path)
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-model", str(model),
              "--online-model-no-provenance", "--lpq-bucket-width", "20")
    learner = doc["lpq"]["learner"]
    assert learner["attached"] is True
    assert learner["events_recorded"] > 0
    assert learner["accounting_balanced"] is True
    assert learner["apply_policy"] == "between_queries"
    assert learner["label_definition"] == "online_first_choice_extractable_v1"
    assert learner["model"]["compatibility"]["suitability"] == "not assessed"
    assert learner["model"]["compatibility"]["overrides"] == ["no_provenance"]
    # The legacy flag is derived from the measured block, not configured.
    assert doc["lpq"]["online_learning"] is (learner["updates_applied"] > 0 and learner["weights_changed"])
    assert "improv" not in json.dumps(learner).replace("not an improvement", "")


def test_a_disabled_learner_records_events_but_applies_nothing_and_says_so(run_tool, tmp_path):
    model = _model(tmp_path)
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-model", str(model),
              "--online-model-no-provenance", "--online-update-mode", "disabled",
              "--lpq-bucket-width", "20")
    learner = doc["lpq"]["learner"]
    assert learner["events_recorded"] > 0
    assert learner["updates_applied"] == 0
    assert learner["weights_changed"] is False
    assert learner["weights_hash_start"] == learner["weights_hash_end"]
    assert doc["lpq"]["online_learning"] is False


def test_a_cold_start_needs_no_artifact_and_is_labelled(run_tool):
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-start", "cold",
              "--lpq-bucket-width", "20")
    model = doc["lpq"]["learner"]["model"]
    assert model["source"] == "zero_initialised"
    assert doc["correctness"]["exact_match"] is True


# -----------------------------------------------------------------------------
# D-12: lpq_adaptive's learner records events and nothing else
# -----------------------------------------------------------------------------


def test_the_old_adaptive_learner_is_reported_as_attached_but_not_learning(run_tool):
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_adaptive", "--adaptive-learning")
    learner = doc["lpq"]["learner"]
    assert learner["attached"] is True
    assert learner["events_recorded"] > 0
    assert learner["updates_applied"] == 0
    assert learner["weights_changed"] is False
    assert doc["lpq"]["online_learning"] is False, "the flag used to read true here"
    assert doc["lpq"]["scorer"] == "constant_control_no_model"


def test_queues_with_no_learner_say_so(run_tool, tmp_path):
    for args in (["--pq", "lpq"], ["--pq", "lpq_model", "--model", str(_model(tmp_path))]):
        learner = _ok(run_tool, _queries(1)[0], *args)["lpq"]["learner"]
        assert learner["attached"] is False
        assert learner["events_recorded"] == 0


# -----------------------------------------------------------------------------
# A model is checked before any query runs
# -----------------------------------------------------------------------------


def _refused(tool: str, *args: str) -> dict:
    result = _run(tool, _queries(1)[0], "--pq", "lpq_online", *args)
    assert result.returncode != 0
    doc = json.loads(result.stdout)
    assert doc["status"] == "failed"
    return doc["error"]


def test_there_is_no_default_model(run_tool):
    error = _refused(run_tool)
    assert error["category"] == "invalid_request"
    assert "--online-model" in error["message"]


def test_a_model_with_no_report_is_refused_unless_waived(run_tool, tmp_path):
    error = _refused(run_tool, "--online-model", str(_model(tmp_path)))
    assert error["category"] == "model"
    assert "no model report" in error["message"]


def test_a_report_for_another_graph_is_refused_and_the_override_is_recorded(run_tool, tmp_path):
    model, report = _model(tmp_path), _report(tmp_path, graph_checksum=12345)
    error = _refused(run_tool, "--online-model", str(model), "--online-model-report", str(report),
                     "--lpq-bucket-width", "20")
    assert "different graph" in error["message"]
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-model", str(model),
              "--online-model-report", str(report), "--allow-graph-mismatch",
              "--lpq-bucket-width", "20")
    compat = doc["lpq"]["learner"]["model"]["compatibility"]
    assert compat["graph_match"] == "mismatch_overridden"
    assert "graph_mismatch" in compat["overrides"]
    assert compat["suitability"] == "not assessed", "an override is not a statement of suitability"


def test_a_report_for_another_bucket_width_needs_its_own_override(run_tool, tmp_path):
    model = _model(tmp_path)
    report = _report(tmp_path, bucket_width=7)
    base = ["--online-model", str(model), "--online-model-report", str(report),
            "--lpq-bucket-width", "20"]
    assert "bucket width" in _refused(run_tool, *base)["message"]
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", *base, "--allow-width-mismatch")
    assert doc["lpq"]["learner"]["model"]["compatibility"]["width_match"] == "mismatch_overridden"


def test_a_report_missing_a_field_it_is_checked_against_is_refused(run_tool, tmp_path):
    model = _model(tmp_path)
    report = _report(tmp_path, bucket_width=None)
    error = _refused(run_tool, "--online-model", str(model), "--online-model-report", str(report),
                     "--lpq-bucket-width", "20")
    assert "no bucket_width" in error["message"]


def test_a_matching_report_is_recorded_with_its_provenance(run_tool, tmp_path):
    model = _model(tmp_path)
    report = _report(tmp_path, bucket_width=20)
    doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-model", str(model),
              "--online-model-report", str(report), "--lpq-bucket-width", "20")
    compat = doc["lpq"]["learner"]["model"]["compatibility"]
    assert (compat["graph_match"], compat["width_match"], compat["overrides"]) == ("match", "match", [])
    assert doc["lpq"]["learner"]["model"]["report"]["dataset_id"] == "demo_road"
    assert doc["lpq"]["learner"]["model"]["artifact_bytes"] == model.stat().st_size


def test_a_weight_on_a_feature_the_scorer_never_supplies_is_refused(run_tool, tmp_path):
    weights = expand_static_weights([1.0, 0, 0, 0, 0, 0])
    weights[20] = 0.5  # a dynamic-feature column
    path = tmp_path / "bad.lpqmodel"
    write_linear_model(path, weights, 0.0, kind=ModelKind.LOGISTIC)
    error = _refused(run_tool, "--online-model", str(path), "--online-model-no-provenance")
    assert error["category"] == "model"
    assert "static scorer does not supply" in error["message"]


def test_a_malformed_report_and_a_corrupt_model_are_refused(run_tool, tmp_path):
    model = _model(tmp_path)
    broken = tmp_path / "broken.report.json"
    broken.write_text('{"graph_checksum": ', encoding="utf-8")
    assert "not valid JSON" in _refused(run_tool, "--online-model", str(model),
                                        "--online-model-report", str(broken))["message"]
    garbage = tmp_path / "garbage.lpqmodel"
    garbage.write_bytes(b"not a model at all, just bytes")
    error = _refused(run_tool, "--online-model", str(garbage), "--online-model-no-provenance")
    assert error["category"] == "model"
    assert "model rejected" in error["message"]


def test_online_flags_are_refused_for_other_queues_and_in_search_updates_do_not_exist(run_tool, tmp_path):
    result = _run(run_tool, _queries(1)[0], "--pq", "lpq", "--online-start", "cold")
    assert result.returncode != 0 and "lpq_online" in result.stdout
    error = _refused(run_tool, "--online-model", str(_model(tmp_path)), "--online-apply", "in_search")
    assert "not implemented" in error["message"]


def test_the_artifact_hash_recorded_by_the_engine_identifies_the_bytes(run_tool, tmp_path):
    a, b = _model(tmp_path, name="a"), _model(tmp_path, bias=-1.5, name="b")
    assert sha256_file(a) != sha256_file(b)
    hashes = []
    for model in (a, b):
        doc = _ok(run_tool, _queries(1)[0], "--pq", "lpq_online", "--online-model", str(model),
                  "--online-model-no-provenance", "--lpq-bucket-width", "20")
        hashes.append(doc["lpq"]["learner"]["model"]["artifact_fnv1a64"])
    assert hashes[0] != hashes[1]
