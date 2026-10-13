"""lpq_benchmark with the online learner (M5-O6).

The model of an ``lpq_online`` row is constant during a query, so every timed repetition of that query
runs on the state it started from; the events the validation run recorded are applied afterwards. So
the state advances once per query, in manifest order, whatever the repetition count and the
interleaving, and a frozen row is the ``lpq_model`` row run for run. These tests check exactly that,
and that the mode is explicit and the model is vetted. They skip, with the command that fixes it, when
``lpq_benchmark`` or the demo graph is not built.
"""

from __future__ import annotations

import json
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
_COUNT = 30  # a prefix of the demo manifest: enough for a trajectory, cheap enough for a test


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


@pytest.fixture
def model(tmp_path: Path) -> Path:
    path = tmp_path / "m.lpqmodel"
    write_linear_model(path, expand_static_weights([1.5, -0.4, 0.8, -0.2, 0.3, 0.6]), -1.0,
                       kind=ModelKind.LOGISTIC)
    return path


def _bench(tool: str, queries: Path, tmp_path: Path, name: str, *args: str, expect_ok: bool = True):
    out = tmp_path / f"{name}.json"
    result = subprocess.run(
        [tool, "--dataset", str(_GRAPH), "--queries-file", str(queries), "--warmup", "0",
         "--lpq-bucket-width", "20", "--output", str(out), *args],
        capture_output=True, text=True, check=False,
    )
    if expect_ok:
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads(out.read_text())["records"]
    assert result.returncode != 0
    return result.stdout + result.stderr


ONLINE = ["--pq", "lpq_online", "--evaluation", "online_session", "--online-model-no-provenance"]


def _trajectory(records, label="lpq_online"):
    return [(r["learner"]["query_index"], r["learner"]["weights_hash_before"],
             r["learner"]["weights_hash_after"]) for r in records if r["pq"] == label]


def test_a_frozen_online_row_is_the_model_row_query_for_query(tool, queries, tmp_path, model):
    frozen = _bench(tool, queries, tmp_path, "model", "--pq", "lpq_model", "--model", str(model),
                    "--repetitions", "1")
    online = _bench(tool, queries, tmp_path, "online", *ONLINE, "--model", str(model),
                    "--online-update-mode", "disabled", "--repetitions", "1")
    assert len(frozen) == len(online) == _COUNT
    for a, b in zip(frozen, online, strict=True):
        assert a["pq_operations"] == b["pq_operations"]
        assert b["correctness"]["exact_match"] is True
        assert "learner" not in a
    hashes = {h for _, before, after in _trajectory(online) for h in (before, after)}
    assert len(hashes) == 1, "a disabled learner never moves the weights"


def test_updates_between_queries_leave_each_querys_search_unchanged_and_carry_state(
    tool, queries, tmp_path, model
):
    frozen = _bench(tool, queries, tmp_path, "model", "--pq", "lpq_model", "--model", str(model),
                    "--repetitions", "1")
    online = _bench(tool, queries, tmp_path, "online", *ONLINE, "--model", str(model), "--repetitions", "1")
    traj = _trajectory(online)
    assert [q for q, _, _ in traj] == list(range(_COUNT)), "manifest order, once each"
    for (_, _, after), (_, before, _) in zip(traj, traj[1:], strict=False):
        assert after == before, "the state a query ends with is the state the next one starts with"
    assert traj[0][1] != traj[-1][2], "the weights did move over the session"
    assert all(r["correctness"]["exact_match"] for r in online)
    # Query 0 ran on the start model, so it is the frozen row's query 0 exactly.
    assert online[0]["pq_operations"]["comparisons"] == frozen[0]["pq_operations"]["comparisons"]
    assert all(r["learner"]["accounting_balanced"] for r in online)
    assert all(r["ml"]["online_update_ns"] > 0 for r in online if r["learner"]["events_applied"])


def test_the_trajectory_does_not_depend_on_repetitions_or_interleaving(tool, queries, tmp_path, model):
    base = ["--model", str(model), "--online-update-mode", "periodic"]
    one = _bench(tool, queries, tmp_path, "r1", *ONLINE, *base, "--repetitions", "1")
    three = _bench(tool, queries, tmp_path, "r3", *ONLINE, *base, "--repetitions", "3")
    assert _trajectory(one) == _trajectory(three), "repetitions replay the same state"
    assert [r["pq_operations"]["comparisons"] for r in one] == [
        r["pq_operations"]["comparisons"] for r in three]
    # Two rows, interleaved (query-major, shuffled queue order per query): each row still advances
    # once per query, in manifest order.
    pair = ["--evaluation", "online_session", "--online-model-no-provenance", "--model", str(model),
            "--compare", "lpq_online@a;lpq_online@b", "--repetitions", "1"]
    plain = _bench(tool, queries, tmp_path, "pair", *pair)
    mixed = _bench(tool, queries, tmp_path, "pair_i", *pair, "--interleave")
    for label in ("a", "b"):
        assert _trajectory(plain, label) == _trajectory(mixed, label) == _trajectory(one)


def test_the_evaluation_mode_must_be_explicit_and_match_the_run(tool, queries, tmp_path, model):
    text = _bench(tool, queries, tmp_path, "x", "--pq", "lpq_online", "--model", str(model),
                  "--online-model-no-provenance", expect_ok=False)
    assert "--evaluation online_session" in text
    text = _bench(tool, queries, tmp_path, "y", "--pq", "lpq_model", "--model", str(model),
                  "--evaluation", "online_session", expect_ok=False)
    assert "only apply to a run that contains lpq_online" in text


def test_the_model_is_vetted_and_overrides_are_recorded(tool, queries, tmp_path, model):
    text = _bench(tool, queries, tmp_path, "np", "--pq", "lpq_online", "--evaluation", "online_session",
                  "--model", str(model), expect_ok=False)
    assert "no model report" in text
    meta = json.loads((_PATHS.root / "datasets" / "metadata" / "demo_road.json").read_text())
    report = tmp_path / "m.report.json"
    report.write_text(json.dumps({"dataset_id": "demo_road", "graph_checksum": 1, "bucket_width": 20}))
    base = ["--pq", "lpq_online", "--evaluation", "online_session", "--model", str(model),
            "--online-model-report", str(report), "--repetitions", "1"]
    assert "different graph" in _bench(tool, queries, tmp_path, "gm", *base, expect_ok=False)
    records = _bench(tool, queries, tmp_path, "ov", *base, "--allow-graph-mismatch")
    learner = records[0]["learner"]
    assert learner["graph_match"] == "mismatch_overridden"
    assert learner["overrides"] == "graph_mismatch"
    assert learner["suitability"] == "not assessed"
    assert learner["label_definition"] == "online_first_choice_extractable_v1"
    report.write_text(json.dumps({"dataset_id": "demo_road",
                                  "graph_checksum": meta["graph_checksum_fnv1a64"],
                                  "bucket_width": 20}))
    ok = _bench(tool, queries, tmp_path, "match", *base)
    assert (ok[0]["learner"]["graph_match"], ok[0]["learner"]["overrides"]) == ("match", "")
