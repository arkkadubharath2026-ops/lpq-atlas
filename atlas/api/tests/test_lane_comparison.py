"""The fair multi-lane comparison endpoint, against the real engine.

Timing values are whatever the engine measured on this machine; these tests check
structure and internal consistency (the API's arithmetic on the engine's numbers,
sequencing, order reporting, the same-distance check), never a speed claim.
"""

from __future__ import annotations

import json
import time
from typing import Any

import pytest

from .conftest import FIXTURE_ID, PART

VANILLA_D = {"id": "vanilla_dijkstra", "algorithm": "dijkstra", "queue": "binary_heap"}
VANILLA_A = {"id": "vanilla_astar", "algorithm": "astar", "queue": "binary_heap"}
RADIX = {"id": "radix_dijkstra", "algorithm": "dijkstra", "queue": "radix_heap"}
OPT_D = {"id": "optimized_dijkstra", "algorithm": "dijkstra", "queue": "lpq_optimized"}
OPT_A = {"id": "optimized_astar", "algorithm": "astar", "queue": "lpq_optimized"}


def _engine_has(client: Any, queue: str) -> bool:
    q = next(q for q in client.get("/api/queues").json() if q["id"] == queue)
    return q["engine_available"] is True


def _wait(client: Any, cmp_id: str, timeout: float = 120.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = client.get(f"/api/lane-comparisons/{cmp_id}").json()
        if d["state"] in ("COMPLETED", "FAILED", "CANCELLED"):
            return d
        time.sleep(0.1)
    raise AssertionError("lane comparison did not finish")


def _compare(client: Any, lanes: list[dict[str, Any]], **body: Any) -> dict[str, Any]:
    r = client.post(
        "/api/lane-comparisons",
        json={
            "dataset_id": FIXTURE_ID,
            "source_node": 0,
            "target_node": PART - 1,
            "lanes": lanes,
            **body,
        },
    )
    assert r.status_code == 202, r.text
    d = _wait(client, r.json()["id"])
    assert d["state"] == "COMPLETED", d
    return d


def test_classical_lanes_are_timed_fairly(client: Any) -> None:
    d = _compare(client, [VANILLA_D, VANILLA_A, RADIX], order_seed=7)
    res = d["result"]
    m = res["measurement"]
    # Defaults: 7 timed repetitions after 2 warm-ups, tracing off, one lane at a time.
    assert (m["repetitions"], m["warmup"], m["tracing"]) == (7, 2, "off")
    assert m["order_seed"] == 7
    assert sorted(m["run_order"]) == sorted(ln["id"] for ln in res["lanes"])
    lanes = {ln["id"]: ln for ln in res["lanes"]}
    for ln in lanes.values():
        assert ln["status"] == "ok", ln
        t = ln["timing"]
        assert len(t["samples_ns"]) == 7 and t["repetitions"] == 7 and t["warmup"] == 2
        assert t["median_ns"] > 0 and t["mad_ns"] >= 0
        assert (ln["median_ns"], ln["mad_ns"]) == (t["median_ns"], t["mad_ns"])
        assert ln["counts"]["settled_nodes"] > 0 and ln["counts"]["pops"] >= 1
        assert ln["matches_reference"] is True
        assert m["run_order"].index(ln["id"]) + 1 == ln["run_index"]
    # Same distance everywhere, and the engine's own reference agrees.
    assert res["correctness"]["all_match_reference"] is True
    assert res["correctness"]["all_lanes_same_distance"] is True
    assert len({ln["distance"] for ln in lanes.values()}) == 1
    # The API's speedup is exactly vanilla_median / lane_median.
    vd = lanes["vanilla_dijkstra"]["timing"]["median_ns"]
    assert lanes["vanilla_dijkstra"]["speedup_vs_vanilla"] == 1.0
    assert lanes["radix_dijkstra"]["speedup_vs_vanilla"] == pytest.approx(
        vd / lanes["radix_dijkstra"]["timing"]["median_ns"]
    )
    assert lanes["vanilla_astar"]["vanilla_lane_id"] == "vanilla_astar"
    fastest = min(lanes.values(), key=lambda ln: ln["timing"]["median_ns"])
    assert res["fastest"]["lane_id"] == fastest["id"]
    assert lanes[fastest["id"]]["rank"] == 1
    # A* has coordinates here, so it uses the geometric heuristic; Dijkstra uses none.
    assert res["heuristic"]["used_by_astar_lanes"] == "haversine"
    assert lanes["vanilla_astar"]["heuristic_used"] == "haversine"
    assert res["path"][0] == 0 and res["path"][-1] == PART - 1


def test_every_lane_uses_the_same_query_counts_and_no_tracing(client: Any) -> None:
    d = _compare(client, [VANILLA_D, RADIX], repetitions=3, warmup=2)
    # The engine log records the exact argv each lane ran with.
    from atlas_api.app import lane_comparisons  # imported after the client set the env

    job_dir = lane_comparisons.get(d["id"]).dir
    argvs = []
    for lane_id in ("vanilla_dijkstra", "radix_dijkstra"):
        first = (job_dir / f"{lane_id}.log").read_text().splitlines()[0]
        argvs.append(json.loads(first.removeprefix("argv: ")))
    for argv in argvs:
        flag = lambda name, a=argv: a[a.index(name) + 1]
        assert flag("--trace-level") == "none" and "--trace-out" not in argv
        assert (flag("--repetitions"), flag("--warmup")) == ("3", "2")
        assert (flag("--source"), flag("--target")) == ("0", str(PART - 1))
    assert len({a[a.index("--dataset") + 1] for a in argvs}) == 1  # one graph for every lane


def test_run_order_is_shuffled_per_request_and_reproducible_from_the_seed(client: Any) -> None:
    lanes = [VANILLA_D, VANILLA_A, RADIX]
    orders = {
        seed: _compare(client, lanes, order_seed=seed, repetitions=3)["result"]["measurement"][
            "run_order"
        ]
        for seed in (1, 2, 3, 4, 5, 6)
    }
    assert all(sorted(o) == sorted(ln["id"] for ln in lanes) for o in orders.values())
    assert len({tuple(o) for o in orders.values()}) > 1  # not a fixed order
    again = _compare(client, lanes, order_seed=3, repetitions=3)["result"]["measurement"]
    assert again["run_order"] == orders[3]
    unseeded = _compare(client, lanes, repetitions=3)["result"]["measurement"]
    assert isinstance(unseeded["order_seed"], int)  # chosen and reported, so replayable


def test_unreachable_query_agrees_across_lanes(client: Any) -> None:
    d = _compare(client, [VANILLA_D, RADIX], target_node=PART + 3, repetitions=3)
    res = d["result"]
    assert res["correctness"]["reachable"] is False
    assert res["correctness"]["all_match_reference"] is True
    assert all(ln["distance"] is None for ln in res["lanes"])


@pytest.mark.parametrize(
    "body",
    [
        {"lanes": [VANILLA_D]},
        {"lanes": [VANILLA_D, {**RADIX, "id": VANILLA_D["id"]}]},
        {"lanes": [VANILLA_D, {**VANILLA_D, "id": "again"}]},
        {"lanes": [VANILLA_D, RADIX], "repetitions": 2},
        {"lanes": [VANILLA_D, RADIX], "warmup": 1},
        {"lanes": [VANILLA_D, {"id": "m", "algorithm": "dijkstra", "queue": "lpq_model"}]},
        {"lanes": [VANILLA_D, {"id": "x", "algorithm": "dijkstra", "queue": "nope"}]},
        {"lanes": [VANILLA_D, RADIX], "heuristic": "zero"},
        {"lanes": [VANILLA_D, {**RADIX, "id": "../x"}]},
    ],
)
def test_malformed_lane_requests_are_rejected(client: Any, body: dict[str, Any]) -> None:
    base = {"dataset_id": FIXTURE_ID, "source_node": 0, "target_node": 1}
    r = client.post("/api/lane-comparisons", json={**base, **body})
    assert r.status_code == 422, r.text


def test_out_of_range_node_and_incompatible_heuristic_are_400(client: Any) -> None:
    base = {"dataset_id": FIXTURE_ID, "source_node": 0, "lanes": [VANILLA_D, VANILLA_A]}
    assert client.post("/api/lane-comparisons", json={**base, "target_node": 10**6}).status_code == 400
    r = client.post(
        "/api/lane-comparisons", json={**base, "target_node": 1, "heuristic": "euclidean"}
    )
    assert r.status_code == 400  # fixture coordinates are geographic


def test_optimized_queue_is_registered_and_needs_no_model(client: Any) -> None:
    q = next(q for q in client.get("/api/queues").json() if q["id"] == "lpq_optimized")
    assert q["family"] == "learned" and q["name"] == "Optimized (ML-assisted)"
    assert not q.get("requires_model") and q["description"]
    assert q["engine_available"] in (True, False, None)
    base = {"dataset_id": FIXTURE_ID, "source_node": 0, "target_node": 1, "queue": "lpq_optimized"}
    for extra in (
        {"model_id": "m"},
        {"correctness_policy": "approximate", "epsilon": 0.1},
        {"lpq_window": [2, 8]},
        {"lpq_bucket_width": 10},
        {"adaptive_learning": True},
    ):
        assert client.post("/api/runs", json={**base, **extra}).status_code == 422, extra


def test_lane_whose_engine_cannot_run_fails_alone(client: Any) -> None:
    if _engine_has(client, "lpq_optimized"):
        pytest.skip("this lpq_run provides lpq_optimized; the failure path cannot be provoked")
    d = _compare(client, [VANILLA_D, OPT_D], repetitions=3)
    lanes = {ln["id"]: ln for ln in d["result"]["lanes"]}
    assert lanes["optimized_dijkstra"]["status"] == "error"
    assert lanes["optimized_dijkstra"]["error"]["message"]
    assert lanes["optimized_dijkstra"]["timing"] is None
    assert lanes["optimized_dijkstra"]["speedup_vs_vanilla"] is None
    assert lanes["vanilla_dijkstra"]["status"] == "ok"
    assert d["result"]["correctness"]["failed_lanes"] == ["optimized_dijkstra"]
    assert d["result"]["fastest"]["lane_id"] == "vanilla_dijkstra"


def test_optimized_lanes_return_the_exact_distance(client: Any) -> None:
    if not _engine_has(client, "lpq_optimized"):
        pytest.skip("lpq_run does not list lpq_optimized; rebuild the engine to run this")
    d = _compare(client, [VANILLA_D, VANILLA_A, OPT_D, OPT_A, RADIX])
    res = d["result"]
    assert all(ln["status"] == "ok" for ln in res["lanes"]), res["lanes"]
    assert res["correctness"]["all_match_reference"] is True
    assert res["correctness"]["all_lanes_same_distance"] is True
    lanes = {ln["id"]: ln for ln in res["lanes"]}
    assert lanes["optimized_astar"]["vanilla_lane_id"] == "vanilla_astar"
    assert lanes["optimized_astar"]["heuristic_used"] == lanes["vanilla_astar"]["heuristic_used"]
    kinds = {(e["kind"], e["lane_id"]) for e in res["effects"]}
    assert ("ml", "optimized_dijkstra") in kinds and ("algorithm", "vanilla_astar") in kinds
    r = client.post(
        "/api/runs",
        json={
            "dataset_id": FIXTURE_ID,
            "source_node": 0,
            "target_node": PART - 1,
            "queue": "lpq_optimized",
            "repetitions": 1,
            "warmup": 0,
        },
    )
    assert r.status_code == 202


def test_single_runs_and_lane_comparisons_do_not_overlap(client: Any) -> None:
    """A timed single run takes the same exclusive slot, so it queues behind a comparison."""
    import threading

    from atlas_api import engine

    held = threading.Event()
    release = threading.Event()

    def holder() -> None:
        with engine._TIMING_LOCK:
            held.set()
            release.wait(10)

    t = threading.Thread(target=holder)
    t.start()
    assert held.wait(5)
    try:
        r = client.post(
            "/api/runs",
            json={
                "dataset_id": FIXTURE_ID,
                "source_node": 0,
                "target_node": PART - 1,
                "trace_level": "none",
                "repetitions": 2,
                "warmup": 0,
            },
        )
        run_id = r.json()["id"]
        time.sleep(0.6)
        assert client.get(f"/api/runs/{run_id}").json()["state"] != "COMPLETED"
        release.set()
        t.join()
        assert _wait_run(client, run_id) == "COMPLETED"
    finally:
        release.set()


def _wait_run(client: Any, run_id: str, timeout: float = 60.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = client.get(f"/api/runs/{run_id}").json()["state"]
        if s in ("COMPLETED", "FAILED", "CANCELLED"):
            return s
        time.sleep(0.1)
    raise AssertionError("run did not finish")
