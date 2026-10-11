"""Scoring of a lane comparison: pure arithmetic, no engine needed.

The speedup, ranking, fastest-lane and same-distance figures are computed once, in
atlas_api.lanes, and the web page only displays them. These tests pin that logic
with synthetic per-lane facts, so they cannot be mistaken for measurements.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import pytest

from atlas_api import lanes as L


def lane(
    lane_id: str,
    algorithm: str,
    queue: str,
    median: float,
    *,
    mad: float = 0.0,
    distance: int | None = 100,
    family: str | None = None,
    status: str = "ok",
) -> dict[str, Any]:
    if status != "ok":
        return {
            "id": lane_id,
            "algorithm": algorithm,
            "queue": queue,
            "family": family or ("learned" if queue.startswith("lpq") else "classical"),
            "status": "error",
            "error": {"category": "engine", "message": "boom"},
            "timing": None,
        }
    return {
        "id": lane_id,
        "algorithm": algorithm,
        "queue": queue,
        "family": family or ("learned" if queue.startswith("lpq") else "classical"),
        "status": "ok",
        "timing": {"median_ns": median, "mad_ns": mad},
        "distance": distance,
        "reachable": distance is not None,
        "reference": {"reachable": distance is not None, "distance": distance},
    }


def four_lanes(opt_d: float = 800.0, opt_a: float = 450.0) -> list[dict[str, Any]]:
    return [
        lane("vd", "dijkstra", "binary_heap", 1000.0),
        lane("va", "astar", "binary_heap", 500.0),
        lane("od", "dijkstra", "lpq_optimized", opt_d),
        lane("oa", "astar", "lpq_optimized", opt_a),
    ]


def by_id(lanes: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {ln["id"]: ln for ln in lanes}


def test_speedup_is_vanilla_median_over_lane_median() -> None:
    lanes = four_lanes(opt_d=800.0, opt_a=250.0)
    L.analyse(lanes)
    b = by_id(lanes)
    assert b["vd"]["speedup_vs_vanilla"] == 1.0 and b["vd"]["verdict"] == "reference"
    assert b["od"]["speedup_vs_vanilla"] == pytest.approx(1000.0 / 800.0)
    assert b["od"]["verdict"] == "faster"
    # An A* lane is judged against vanilla A*, not against vanilla Dijkstra.
    assert b["oa"]["vanilla_lane_id"] == "va"
    assert b["oa"]["speedup_vs_vanilla"] == pytest.approx(500.0 / 250.0)
    assert b["va"]["speedup_vs_vanilla"] == 1.0


def test_slower_lane_is_reported_slower_not_spun() -> None:
    lanes = four_lanes(opt_d=1500.0)
    L.analyse(lanes)
    od = by_id(lanes)["od"]
    assert od["verdict"] == "slower"
    assert od["speedup_vs_vanilla"] == pytest.approx(1000.0 / 1500.0)
    assert od["speedup_vs_vanilla"] < 1.0


def test_difference_inside_the_noise_is_not_called_a_win() -> None:
    lanes = [
        lane("vd", "dijkstra", "binary_heap", 1000.0, mad=60.0),
        lane("od", "dijkstra", "lpq_optimized", 950.0, mad=60.0),
    ]
    L.analyse(lanes)
    od = by_id(lanes)["od"]
    assert od["speedup_vs_vanilla"] == pytest.approx(1000.0 / 950.0)  # still reported
    assert od["verdict"] == "same" and od["within_noise"] is True


def test_tiny_difference_is_same_even_with_zero_mad() -> None:
    lanes = [
        lane("vd", "dijkstra", "binary_heap", 1000.0),
        lane("od", "dijkstra", "lpq_optimized", 990.0),
    ]
    L.analyse(lanes)
    assert by_id(lanes)["od"]["verdict"] == "same"


def test_fastest_and_rank_follow_median_and_tie_is_reported() -> None:
    lanes = four_lanes(opt_d=800.0, opt_a=250.0)
    s = L.analyse(lanes)
    assert s["fastest"]["lane_id"] == "oa"
    assert s["fastest"]["clear_win"] is True
    ranks = {ln["id"]: ln["rank"] for ln in lanes}
    assert ranks == {"oa": 1, "va": 2, "od": 3, "vd": 4}
    tied = [
        lane("vd", "dijkstra", "binary_heap", 1000.0, mad=80.0),
        lane("rd", "dijkstra", "radix_heap", 960.0, mad=80.0),
    ]
    s2 = L.analyse(tied)
    assert s2["fastest"]["lane_id"] == "rd"
    assert s2["fastest"]["tied_with"] == ["vd"] and s2["fastest"]["clear_win"] is False


def test_wrong_distance_cannot_win_and_breaks_the_check() -> None:
    lanes = [
        lane("vd", "dijkstra", "binary_heap", 1000.0, distance=100),
        lane("od", "dijkstra", "lpq_optimized", 10.0, distance=100),
    ]
    lanes[1]["distance"] = 99  # engine reference still says 100
    s = L.analyse(lanes)
    b = by_id(lanes)
    assert b["od"]["matches_reference"] is False
    assert b["od"]["rank"] is None
    assert s["fastest"]["lane_id"] == "vd"
    assert s["correctness"]["all_match_reference"] is False
    assert s["correctness"]["all_lanes_same_distance"] is False
    assert s["correctness"]["mismatching_lanes"] == ["od"]


def test_all_lanes_agreeing_sets_the_flag() -> None:
    lanes = four_lanes()
    s = L.analyse(lanes)
    assert s["correctness"]["all_match_reference"] is True
    assert s["correctness"]["all_lanes_same_distance"] is True
    assert all(ln["matches_reference"] is True for ln in lanes)


def test_failed_lane_does_not_break_the_others() -> None:
    lanes = four_lanes()
    lanes[2] = lane("od", "dijkstra", "lpq_optimized", 0, status="error")
    s = L.analyse(lanes)
    b = by_id(lanes)
    assert b["od"]["matches_reference"] is None and b["od"]["rank"] is None
    assert b["od"]["speedup_vs_vanilla"] is None and b["od"]["verdict"] == "unavailable"
    assert s["correctness"]["failed_lanes"] == ["od"]
    assert s["correctness"]["all_match_reference"] is True  # the three that ran agree
    assert s["fastest"] is not None


def test_no_vanilla_lane_means_no_speedup() -> None:
    lanes = [
        lane("rd", "dijkstra", "radix_heap", 900.0),
        lane("od", "dijkstra", "lpq_optimized", 800.0),
    ]
    L.analyse(lanes)
    assert all(ln["speedup_vs_vanilla"] is None for ln in lanes)


def test_effects_separate_algorithm_queue_and_ml() -> None:
    lanes = four_lanes(opt_d=800.0, opt_a=250.0) + [
        lane("rd", "dijkstra", "radix_heap", 600.0),
    ]
    s = L.analyse(lanes)
    fx = {(e["kind"], e["lane_id"]): e for e in s["effects"]}
    assert fx[("algorithm", "va")]["speedup"] == pytest.approx(1000.0 / 500.0)
    assert fx[("algorithm", "va")]["versus_lane_id"] == "vd"
    assert fx[("queue", "rd")]["speedup"] == pytest.approx(1000.0 / 600.0)
    assert fx[("ml", "od")]["speedup"] == pytest.approx(1000.0 / 800.0)
    assert fx[("ml", "oa")]["versus_lane_id"] == "va"
    # Against the best classical Dijkstra queue (radix), the ML lane is slower.
    best = fx[("ml_vs_best_classical", "od")]
    assert best["versus_lane_id"] == "rd" and best["verdict"] == "slower"
    assert best["speedup"] == pytest.approx(600.0 / 800.0)


def test_unreachable_query_counts_as_agreement() -> None:
    lanes = [
        lane("vd", "dijkstra", "binary_heap", 100.0, distance=None),
        lane("od", "dijkstra", "lpq_optimized", 90.0, distance=None),
    ]
    s = L.analyse(lanes)
    assert s["correctness"]["all_match_reference"] is True
    assert s["correctness"]["reachable"] is False


# -- shared heuristic, engine queue listing, optimized-queue argv (no engine run) --------


def test_shared_heuristic_resolution() -> None:
    from atlas_api.engine import resolve_shared_heuristic as pick

    assert pick("auto", {"coordinate_system": "geographic"}) == "haversine"
    assert pick("auto", {"coordinate_system": "euclidean"}) == "euclidean"
    assert pick("auto", {"coordinate_system": "none"}) == "zero"
    assert pick("auto", {}) == "zero"
    assert pick("zero", {"coordinate_system": "geographic"}) == "zero"


def test_every_astar_lane_gets_the_same_heuristic_and_every_lane_the_same_query() -> None:
    from atlas_api.engine import lane_run_requests
    from atlas_api.schemas import LaneComparisonRequest

    req = LaneComparisonRequest(
        dataset_id="d",
        source_node=3,
        target_node=9,
        lanes=[
            {"id": "a", "algorithm": "astar", "queue": "binary_heap"},
            {"id": "b", "algorithm": "astar", "queue": "lpq_optimized"},
            {"id": "c", "algorithm": "dijkstra", "queue": "radix_heap"},
        ],
    )
    used, runs = lane_run_requests(req, {"coordinate_system": "geographic"})
    assert used == "haversine"
    assert runs["a"].heuristic == runs["b"].heuristic == "haversine"
    assert runs["c"].heuristic == "auto"  # Dijkstra takes none
    for r in runs.values():
        assert (r.source_node, r.target_node, r.trace_level) == (3, 9, "none")
        assert (r.repetitions, r.warmup) == (7, 2)
        assert r.correctness_policy == "exact" and r.model_id is None


def test_optimized_queue_argv_has_no_model_and_no_tuning_flags() -> None:
    from atlas_api.engine import _engine_flags
    from atlas_api.schemas import EngineConfig

    argv = _engine_flags(EngineConfig(algorithm="dijkstra", queue="lpq_optimized"), None)  # type: ignore[arg-type]
    assert argv == ["--algorithm", "dijkstra", "--pq", "lpq_optimized", "--mode", "exact"]


@pytest.mark.skipif(os.name == "nt", reason="uses a shell script as a fake engine")
@pytest.mark.parametrize(
    ("pq_help", "expected_optimized"),
    [
        ("binary_heap (default) | four_ary_heap | radix_heap | lpq | lpq_model | lpq_adaptive", False),
        (
            ("binary_heap (default) | radix_heap | lpq | lpq_model |\n"
             "                          lpq_adaptive | lpq_optimized"),
            True,
        ),
    ],
)
def test_engine_queue_listing_is_read_from_help(
    tmp_path: Path, pq_help: str, expected_optimized: bool
) -> None:
    from atlas_api.registry import _engine_help_queues

    tool = tmp_path / "lpq_run"
    tool.write_text(
        "#!/bin/sh\ncat <<'EOF'\nlpq_run -- run one query\n\n"
        f"  --algorithm <name>      dijkstra | astar\n  --pq <name>             {pq_help}\n"
        "  --model <path>          trained model; required by lpq_model\n"
        "EOF\n"
    )
    tool.chmod(0o755)
    found = _engine_help_queues(str(tool), 0.0)
    assert found is not None
    assert ("lpq_optimized" in found) is expected_optimized
    assert {"binary_heap", "radix_heap", "lpq", "lpq_model", "lpq_adaptive"} <= found
    assert ("four_ary_heap" in found) is ("four_ary_heap" in pq_help)


def test_unparseable_help_means_unknown_not_unavailable(tmp_path: Path) -> None:
    from atlas_api.registry import _engine_help_queues

    tool = tmp_path / "other_tool"
    tool.write_text("#!/bin/sh\necho nothing useful\n")
    tool.chmod(0o755)
    assert _engine_help_queues(str(tool), 1.0) is None
