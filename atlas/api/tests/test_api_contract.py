"""API <-> engine contract tests (spec section 41) and API behaviour.

The central property: what the API returns for a run IS what the engine
computes when invoked directly with the same inputs -- same distance, same
path, same operation counters, same learned-queue statistics.
"""

from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest

from .conftest import FIXTURE_ID, PART, _tool

DIRECT_KEYS = ("result", "reference", "correctness", "counters", "lpq", "heuristic", "dataset")


def _wait(client: Any, path: str, timeout: float = 60.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = client.get(path).json()
        if d["state"] in ("COMPLETED", "FAILED", "CANCELLED"):
            return d
        time.sleep(0.1)
    raise AssertionError(f"{path} did not finish")


def _run(client: Any, **body: Any) -> tuple[dict[str, Any], dict[str, Any]]:
    body = {
        "dataset_id": FIXTURE_ID,
        "trace_level": "visual",
        "repetitions": 1,
        "warmup": 0,
        **body,
    }
    r = client.post("/api/runs", json=body)
    assert r.status_code == 202, r.text
    job = _wait(client, f"/api/runs/{r.json()['id']}")
    result = (
        client.get(f"/api/runs/{job['id']}/result").json() if job["state"] == "COMPLETED" else {}
    )
    return job, result


def _direct(fixture_root: Path, tmp: Path, args: list[str]) -> dict[str, Any]:
    out = tmp / "direct.json"
    subprocess.run(
        [
            str(_tool("lpq_run")),
            "--dataset",
            str(fixture_root / "datasets" / "processed" / f"{FIXTURE_ID}.lpqgraph"),
            "--repetitions",
            "1",
            "--warmup",
            "0",
            "--result-out",
            str(out),
            *args,
        ],
        check=True,
        capture_output=True,
    )
    return json.loads(out.read_text())


CASES = [
    ({"algorithm": "dijkstra", "queue": q}, ["--algorithm", "dijkstra", "--pq", q])
    for q in ("binary_heap", "four_ary_heap", "pairing_heap", "fibonacci_heap", "radix_heap")
] + [
    (
        {"algorithm": "astar", "queue": "binary_heap"},
        ["--algorithm", "astar", "--pq", "binary_heap"],
    ),
    (
        {"algorithm": "astar", "queue": "radix_heap", "heuristic": "zero"},
        ["--algorithm", "astar", "--pq", "radix_heap", "--heuristic", "zero"],
    ),
    ({"queue": "lpq"}, ["--pq", "lpq"]),
    ({"queue": "lpq_adaptive"}, ["--pq", "lpq_adaptive"]),
    (
        {"queue": "lpq", "correctness_policy": "approximate", "epsilon": 0.25},
        ["--pq", "lpq", "--mode", "approximate", "--epsilon", "0.25"],
    ),
]


@pytest.mark.parametrize(("body", "args"), CASES)
def test_api_result_equals_direct_engine_result(
    client: Any, fixture_root: Path, tmp_path: Path, body: dict[str, Any], args: list[str]
) -> None:
    job, api = _run(client, source_node=0, target_node=PART - 1, **body)
    assert job["state"] == "COMPLETED", job
    direct = _direct(fixture_root, tmp_path, ["--source", "0", "--target", str(PART - 1), *args])
    for key in DIRECT_KEYS:
        assert api.get(key) == direct.get(key), key
    assert api["result"]["reachable"] is True
    assert api["correctness"]["bound_holds"] is True
    assert api["trace"]["matches_untraced_run"] is True


def test_unreachable_pair_has_no_ratio(client: Any) -> None:
    job, res = _run(client, source_node=0, target_node=PART + 3)
    assert job["state"] == "COMPLETED"
    assert res["result"]["reachable"] is False
    assert res["result"]["distance"] is None
    assert res["result"]["path"] == []
    assert res["correctness"]["approximation_ratio"] is None
    assert res["correctness"]["reachability_agrees"] is True


@pytest.mark.parametrize(
    "body",
    [
        {"queue": "binary_heap", "correctness_policy": "approximate", "epsilon": 0.1},
        {"queue": "lpq", "correctness_policy": "approximate", "epsilon": 1.5},
        {"queue": "lpq_model"},
        {"queue": "binary_heap", "model_id": "anything"},
        {"queue": "binary_heap", "lpq_window": [2, 8]},
        {"algorithm": "dijkstra", "heuristic": "haversine"},
        {"queue": "lpq", "epsilon": 0.2},
        {"queue": "not_a_queue"},
        {"dataset_id": "../../etc"},
        {"source_node": -1},
    ],
)
def test_malformed_requests_are_rejected(client: Any, body: dict[str, Any]) -> None:
    base = {"dataset_id": FIXTURE_ID, "source_node": 0, "target_node": 1}
    r = client.post("/api/runs", json={**base, **body})
    assert r.status_code == 422
    assert "details" in r.json()


def test_out_of_range_node_and_unknown_model(client: Any) -> None:
    r = client.post(
        "/api/runs", json={"dataset_id": FIXTURE_ID, "source_node": 0, "target_node": 10**6}
    )
    assert r.status_code == 400
    r = client.post(
        "/api/runs",
        json={
            "dataset_id": FIXTURE_ID,
            "source_node": 0,
            "target_node": 1,
            "queue": "lpq_model",
            "model_id": "missing_model",
        },
    )
    assert r.status_code == 400
    assert client.get("/api/datasets/no_such_dataset").status_code == 404


def test_trace_stream_matches_trace_file_and_result(client: Any) -> None:
    job, res = _run(client, source_node=0, target_node=PART - 1, queue="lpq")
    text = client.get(f"/api/runs/{job['id']}/trace").text
    events = [json.loads(line) for line in text.splitlines()]
    assert [e["seq"] for e in events] == list(range(1, len(events) + 1))
    assert events[0]["type"] == "RUN_STARTED"
    assert events[-1]["type"] == "RUN_COMPLETED"
    path = next(e for e in events if e["type"] == "PATH_RECONSTRUCTED")
    assert path["path"] == res["result"]["path"]
    assert path["distance"] == res["result"]["distance"]
    settled = sum(e["type"] == "NODE_SETTLED" for e in events)
    assert settled == res["counters"]["settled_nodes"]
    accepted = sum(e["type"] == "LPQ_VERIFICATION_ACCEPTED" for e in events)
    assert accepted == res["lpq"]["metrics"]["verified_extractions"]

    streamed: list[dict[str, Any]] = []
    with client.stream("GET", f"/api/runs/{job['id']}/events?batch=50") as s:
        event = None
        for line in s.iter_lines():
            if line.startswith("event:"):
                event = line.split(":", 1)[1].strip()
            elif line.startswith("data:") and event == "trace":
                streamed += json.loads(line[5:])
    assert streamed == events


def test_snap_returns_a_real_graph_node(client: Any) -> None:
    r = client.post(f"/api/graphs/{FIXTURE_ID}/snap", json={"lon": 7.4031, "lat": 43.7019})
    assert r.status_code == 200
    d = r.json()
    node = client.get(f"/api/graphs/{FIXTURE_ID}/nodes/{d['node']}").json()
    assert (node["lon"], node["lat"]) == (d["lon"], d["lat"])
    assert d["snap_distance_m"] < 20


def test_comparison_runs_the_same_query(client: Any) -> None:
    r = client.post(
        "/api/comparisons",
        json={
            "dataset_id": FIXTURE_ID,
            "source_node": 5,
            "target_node": PART - 2,
            "repetitions": 1,
            "warmup": 0,
            "a": {"queue": "binary_heap"},
            "b": {"queue": "lpq"},
        },
    )
    assert r.status_code == 202, r.text
    d = _wait(client, f"/api/comparisons/{r.json()['id']}")
    assert d["state"] == "COMPLETED", d
    assert d["summary"]["same_query"] is True
    assert d["summary"]["reference_agrees"] is True


def test_benchmark_reuses_the_harness(client: Any) -> None:
    r = client.post(
        "/api/benchmarks",
        json={
            "dataset_id": FIXTURE_ID,
            "queues": ["binary_heap", "lpq"],
            "query_count": 4,
            "repetitions": 3,
            "warmup": 0,
        },
    )
    assert r.status_code == 202, r.text
    d = _wait(client, f"/api/benchmarks/{r.json()['id']}", timeout=180)
    assert d["state"] == "COMPLETED", d
    s = client.get(f"/api/benchmarks/{d['id']}/results").json()
    keys = {c["key"] for c in s["configurations"]}
    assert keys == {"dijkstra/binary_heap/exact", "dijkstra/lpq/exact"}
    assert s["records"] == 8
