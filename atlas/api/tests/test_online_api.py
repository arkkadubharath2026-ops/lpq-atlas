"""lpq_online through the API (M5-O7): explicit model, validated options, truthful learner status.

Everything the API returns about the learner is what the engine measured, plus the one fact only the
caller knows (the artifact's SHA-256). Nothing here claims an update helps.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from .conftest import FIXTURE_ID
from .test_api_contract import _run

BODY: dict[str, Any] = {"source_node": 0, "target_node": 10, "lpq_bucket_width": 20}


def _models_dir(fixture_root: Path) -> Path:
    return fixture_root / "models" / "exported"


def _write_model(fixture_root: Path, name: str, *, bias: float = -1.0) -> Path:
    from lpq_tools.training.model_io import (
        ModelKind,
        expand_static_weights,
        write_linear_model,
    )

    path = _models_dir(fixture_root) / f"{name}.lpqmodel"
    write_linear_model(
        path, expand_static_weights([1.5, -0.4, 0.8, -0.2, 0.3, 0.6]), bias, kind=ModelKind.LOGISTIC
    )
    return path


def _write_report(fixture_root: Path, name: str, **fields: Any) -> None:
    meta = json.loads(
        (fixture_root / "datasets" / "metadata" / f"{FIXTURE_ID}.json").read_text("utf-8")
    )
    report = {
        "dataset_id": FIXTURE_ID,
        "graph_checksum": meta["graph_checksum_fnv1a64"],
        "bucket_width": 20,
        "query_count": 10,
        "query_seed": 1,
        "seed": 1,
        **fields,
    }
    (_models_dir(fixture_root) / f"{name}.report.json").write_text(json.dumps(report), "utf-8")


@pytest.fixture(scope="module")
def models(fixture_root: Path) -> dict[str, Path]:
    _models_dir(fixture_root).mkdir(parents=True, exist_ok=True)
    out = {
        "good": _write_model(fixture_root, "online_good"),
        "noreport": _write_model(fixture_root, "online_noreport", bias=-1.1),
        "other": _write_model(fixture_root, "online_other", bias=-1.2),
    }
    _write_report(fixture_root, "online_good")
    _write_report(fixture_root, "online_other", dataset_id="somewhere_else", graph_checksum=12345)
    return out


def _post(client: Any, **body: Any) -> Any:
    return client.post("/api/runs", json={"dataset_id": FIXTURE_ID, **BODY, **body})


def test_a_run_reports_what_the_engine_measured_and_the_artifacts_sha256(client, models):
    job, res = _run(client, **BODY, queue="lpq_online", model_id="online_good")
    assert job["state"] == "COMPLETED", job
    learner = res["lpq"]["learner"]
    assert learner["attached"] is True and learner["events_recorded"] > 0
    assert learner["accounting_balanced"] is True
    model = learner["model"]
    assert model["sha256"] == hashlib.sha256(models["good"].read_bytes()).hexdigest()
    assert model["model_id"] == "online_good"
    compat = model["compatibility"]
    assert (compat["graph_match"], compat["width_match"], compat["overrides"]) == ("match", "match", [])
    assert compat["suitability"] == "not assessed"
    assert res["lpq"]["online_learning"] is (learner["updates_applied"] > 0 and learner["weights_changed"])
    assert res["correctness"]["exact_match"] is True
    assert res["lpq"]["scorer"] == "online_logistic_static_features_on_demand"


@pytest.mark.parametrize("mode", ["disabled", "periodic"])
def test_the_api_search_equals_the_lpq_model_search_with_the_same_artifact(client, models, mode):
    _, frozen = _run(client, **BODY, queue="lpq_model", model_id="online_good")
    _, online = _run(client, **BODY, queue="lpq_online", model_id="online_good",
                     online_update_mode=mode)
    for key in ("result", "counters"):
        assert online[key] == frozen[key]
    assert online["lpq"]["metrics"] == frozen["lpq"]["metrics"]
    if mode == "disabled":
        assert online["lpq"]["learner"]["updates_applied"] == 0
        assert online["lpq"]["online_learning"] is False


def test_a_cold_start_needs_no_model(client, models):
    job, res = _run(client, **BODY, queue="lpq_online", online_start="cold")
    assert job["state"] == "COMPLETED", job
    assert res["lpq"]["learner"]["model"]["source"] == "zero_initialised"


@pytest.mark.parametrize(
    "body",
    [
        {"queue": "lpq_online"},  # no default model
        {"queue": "lpq_online", "online_start": "cold", "model_id": "online_good"},
        {"queue": "lpq_online", "online_start": "cold", "accept_no_provenance": True},
        {"queue": "lpq_online", "model_id": "online_good", "online_update_mode": "sometimes"},
        {"queue": "lpq_online", "model_id": "online_good", "adaptive_learning": True},
        {"queue": "lpq", "online_update_mode": "disabled"},
        {"queue": "lpq_model", "model_id": "online_good", "allow_graph_mismatch": True},
        {"queue": "lpq", "accept_no_provenance": True},
    ],
)
def test_invalid_online_options_are_rejected(client, models, body):
    r = _post(client, **body)
    assert r.status_code == 422, r.text


def test_lpq_online_is_not_offered_to_api_benchmarks(client, models):
    r = client.post(
        "/api/benchmarks",
        json={"dataset_id": FIXTURE_ID, "queues": ["lpq_online", "binary_heap"],
              "model_id": "online_good"},
    )
    assert r.status_code == 422
    assert "evaluation mode" in r.text


def test_an_unknown_model_and_a_model_without_a_report_are_refused_up_front(client, models):
    assert _post(client, queue="lpq_online", model_id="missing").status_code == 400
    r = _post(client, queue="lpq_online", model_id="online_noreport")
    assert r.status_code == 400 and "no report" in r.text


def test_waiving_provenance_is_allowed_and_recorded(client, models):
    job, res = _run(client, **BODY, queue="lpq_online", model_id="online_noreport",
                    accept_no_provenance=True)
    assert job["state"] == "COMPLETED", job
    compat = res["lpq"]["learner"]["model"]["compatibility"]
    assert compat["overrides"] == ["no_provenance"]
    assert compat["graph_match"] == "unknown"


def test_a_model_for_another_dataset_needs_an_explicit_override_and_it_is_recorded(client, models):
    r = _post(client, queue="lpq_online", model_id="online_other")
    assert r.status_code == 400 and "trained on" in r.text
    job, res = _run(client, **BODY, queue="lpq_online", model_id="online_other",
                    allow_graph_mismatch=True)
    assert job["state"] == "COMPLETED", job
    compat = res["lpq"]["learner"]["model"]["compatibility"]
    assert compat["graph_match"] == "mismatch_overridden"
    assert "graph_mismatch" in compat["overrides"]
    assert compat["suitability"] == "not assessed", "an override is not a statement of suitability"


def test_the_engine_refuses_a_width_mismatch_even_when_the_api_lets_it_through(client, models):
    # The report says 20; this request asks for 30. The engine decides, and says why.
    job, _ = _run(client, source_node=0, target_node=10, queue="lpq_online",
                  model_id="online_good", lpq_bucket_width=30)
    assert job["state"] == "FAILED"
    assert "bucket width" in json.dumps(job)


def test_older_queues_keep_their_results_and_say_no_learning_happened(client, models):
    _, adaptive = _run(client, **BODY, queue="lpq_adaptive", adaptive_learning=True)
    learner = adaptive["lpq"]["learner"]
    assert (learner["attached"], learner["updates_applied"]) == (True, 0)
    assert adaptive["lpq"]["online_learning"] is False
    _, plain = _run(client, **BODY, queue="lpq")
    assert plain["lpq"]["learner"]["attached"] is False


def test_the_queue_registry_lists_lpq_online_as_needing_a_model(client, models):
    queues = {q["id"]: q for q in client.get("/api/queues").json()}
    assert queues["lpq_online"]["requires_model"] is True
    assert queues["lpq_online"]["family"] == "learned"
    assert "improv" not in queues["lpq_online"]["description"].replace("helps", "")
