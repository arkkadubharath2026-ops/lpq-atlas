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
    rounds: list[float | None] | None = None,
    resolved_kind: str | None = None,
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
        "timing": {
            "median_ns": median,
            "mad_ns": mad,
            "round_medians_ns": rounds if rounds is not None else [median] * 3,
        },
        "resolved_kind": resolved_kind,
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
        assert (r.repetitions, r.warmup) == (5, 2)
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


# -- measurement rounds: aggregation, majority-sign rule, resolved queue ---------------------


def bench(median: float, mad_ns: float = 0.0, samples: list[int] | None = None) -> dict[str, Any]:
    return {
        "median_ns": median,
        "mad_ns": mad_ns,
        "min_ns": median - 1,
        "max_ns": median + 1,
        "samples_ns": samples or [int(median)] * 5,
        "repetitions": 5,
        "warmup": 2,
        "noisy": False,
        "pinned": True,
        "core": 1,
        "warnings": [],
    }


def test_lane_median_is_the_median_of_round_medians() -> None:
    t = L.aggregate_rounds([bench(10), bench(30), bench(20)])
    assert t is not None
    assert t["median_ns"] == 20
    assert t["round_medians_ns"] == [10, 30, 20]
    assert t["rounds"] == 3 and t["rounds_ok"] == 3
    assert len(t["samples_ns"]) == 15  # every timed sample of every round, nothing else


def test_spread_is_the_larger_of_within_and_between_round_deviation() -> None:
    # Tight within each process, wildly different between processes: the drift shows.
    drift = L.aggregate_rounds([bench(100, 1), bench(140, 1), bench(180, 1)])
    assert drift is not None
    assert drift["mad_within_ns"] == 1
    assert drift["mad_between_rounds_ns"] == 40
    assert drift["mad_ns"] == 40
    # Noisy within each process, identical across rounds: the within figure is kept.
    jitter = L.aggregate_rounds([bench(100, 7), bench(100, 9), bench(100, 8)])
    assert jitter is not None
    assert jitter["mad_between_rounds_ns"] == 0 and jitter["mad_ns"] == 8


def test_a_failed_round_is_left_out_but_keeps_its_slot() -> None:
    t = L.aggregate_rounds([bench(10), None, bench(30)])
    assert t is not None
    assert t["median_ns"] == 20 and t["round_medians_ns"] == [10, None, 30]
    assert t["rounds"] == 3 and t["rounds_ok"] == 2
    assert L.aggregate_rounds([None, None]) is None


def test_win_needs_the_same_sign_in_a_majority_of_rounds() -> None:
    van = lane("vd", "dijkstra", "binary_heap", 100.0, rounds=[100.0] * 4)
    # Median 90 says faster, but the lane is ahead in only 2 of 4 rounds: not a majority.
    split = lane("od", "dijkstra", "lpq_optimized", 90.0, rounds=[50.0, 60.0, 120.0, 130.0])
    L.analyse([van, split])
    assert split["speedup_vs_vanilla"] == pytest.approx(100 / 90)
    assert split["verdict"] == "same"
    assert split["verdict_reason"] == "not_consistent_across_rounds"
    assert (split["rounds_faster"], split["rounds_compared"]) == (2, 4)
    # 3 of 4 is a strict majority.
    most = lane("od", "dijkstra", "lpq_optimized", 70.0, rounds=[50.0, 60.0, 70.0, 130.0])
    van2 = lane("vd", "dijkstra", "binary_heap", 100.0, rounds=[100.0] * 4)
    L.analyse([van2, most])
    assert most["verdict"] == "faster" and most["rounds_faster"] == 3


def test_the_same_rule_stops_a_one_off_loss() -> None:
    van = lane("vd", "dijkstra", "binary_heap", 100.0, rounds=[100.0] * 4)
    split = lane("od", "dijkstra", "lpq_optimized", 110.0, rounds=[80.0, 90.0, 130.0, 140.0])
    L.analyse([van, split])
    assert split["verdict"] == "same" and split["rounds_slower"] == 2


def test_one_round_never_labels_a_win_or_a_loss() -> None:
    van = lane("vd", "dijkstra", "binary_heap", 1000.0, rounds=[1000.0])
    fast = lane("od", "dijkstra", "lpq_optimized", 500.0, rounds=[500.0])
    L.analyse([van, fast])
    assert fast["verdict"] == "same" and fast["verdict_reason"] == "single_round"
    assert fast["speedup_vs_vanilla"] == pytest.approx(2.0)  # still reported, not labelled


def test_consistent_sign_still_needs_to_clear_the_noise() -> None:
    van = lane("vd", "dijkstra", "binary_heap", 1000.0, mad=200.0, rounds=[900.0, 1000.0, 1100.0])
    lane_ = lane("od", "dijkstra", "lpq_optimized", 950.0, mad=200.0, rounds=[850.0, 950.0, 1050.0])
    L.analyse([van, lane_])
    assert lane_["rounds_faster"] == 3
    assert lane_["verdict"] == "same" and lane_["verdict_reason"] == "within_noise"


def test_identical_queues_are_not_split_by_one_noisy_run() -> None:
    a = lane("a", "dijkstra", "radix_heap", 100.0, rounds=[100.0, 160.0, 100.0])
    b = lane("b", "dijkstra", "lpq_optimized", 100.0, rounds=[100.0, 100.0, 100.0],
             resolved_kind="radix_heap")
    van = lane("vd", "dijkstra", "binary_heap", 200.0, rounds=[200.0] * 3)
    s = L.analyse([van, a, b])
    assert s["fastest"]["clear_win"] is False and set(s["fastest"]["tied_with"]) <= {"a", "b"}


def _ok_run(median: float, *, calibration_ns: int = 0, distance: int = 7) -> dict[str, Any]:
    return {
        "status": "ok",
        "error": None,
        "reachable": True,
        "distance": distance,
        "path_hops": 1,
        "path": [0, 1],
        "reference": {"reachable": True, "distance": distance},
        "counts": {"pops": 3},
        "lpq": None,
        "heuristic": None,
        "bench": bench(median),
        "optimizer": {
            "resolved_queue": "radix_heap",
            "resolved_kind": "radix_heap",
            "resolution_method": "autotune_cached",
            "calibration_ns": calibration_ns,
            "reason": "no gain",
            "predicted_ratio_vs_radix": 1.0,
            "tuned_candidates": [],
        },
    }


def _warm(calibration_ns: int, kind: str = "radix_heap") -> dict[str, Any]:
    run = _ok_run(0, calibration_ns=calibration_ns)
    run["bench"] = None  # an untimed call has no benchmark block
    run["optimizer"].update(resolved_kind=kind, resolved_queue=kind, resolution_method="autotune")
    return run


def test_resolved_queue_and_one_time_calibration_come_from_the_warmup_call() -> None:
    from atlas_api.engine import merge_lane_rounds

    lane_ = merge_lane_rounds(_warm(2_500_000_000), [_ok_run(10), _ok_run(12), _ok_run(11)])
    assert lane_["status"] == "ok"
    assert lane_["resolved_queue"] == "radix_heap" and lane_["resolved_to_radix"] is True
    assert lane_["resolution_method"] == "autotune"
    assert lane_["calibration_ns"] == 2_500_000_000
    # The calibration is reported, but is in no timed figure and raises no warning.
    assert lane_["timing"]["median_ns"] == 11
    assert lane_["timing"]["round_medians_ns"] == [10, 12, 11]
    assert len(lane_["timing"]["samples_ns"]) == 15
    assert lane_["timing"]["warnings"] == []


def test_calibration_inside_a_timed_round_is_flagged() -> None:
    from atlas_api.engine import merge_lane_rounds

    lane_ = merge_lane_rounds(_warm(0), [_ok_run(10, calibration_ns=9), _ok_run(11), _ok_run(12)])
    assert any("calibration ran during timed round(s) 1" in w for w in lane_["timing"]["warnings"])


def test_a_lane_that_resolved_to_radix_is_a_queue_choice_not_an_ml_win() -> None:
    from atlas_api.engine import merge_lane_rounds

    opt = merge_lane_rounds(_warm(1), [_ok_run(50)] * 3)
    opt.update(id="od", algorithm="dijkstra", queue="lpq_optimized", family="learned")
    opt["reference"] = {"reachable": True, "distance": 7}
    van = lane("vd", "dijkstra", "binary_heap", 100.0)
    rad = lane("rd", "dijkstra", "radix_heap", 50.0)
    van["reference"] = rad["reference"] = opt["reference"]
    van["distance"] = rad["distance"] = 7
    s = L.analyse([van, rad, opt])
    assert opt["effect"] == "queue"
    kinds = {(e["kind"], e["lane_id"]) for e in s["effects"]}
    assert ("ml", "od") not in kinds
    # It is also set against the radix lane, the strongest non-ML baseline.
    vs_radix = next(e for e in s["effects"] if e["kind"] == "ml_vs_best_classical")
    assert vs_radix["lane_id"] == "od" and vs_radix["versus_lane_id"] == "rd"
    assert vs_radix["verdict"] == "same"


def test_a_lane_that_resolved_to_a_bucket_queue_stays_an_ml_effect() -> None:
    from atlas_api.engine import merge_lane_rounds

    opt = merge_lane_rounds(_warm(1, kind="pooled_bucket"), [_ok_run(50)] * 3)
    assert opt["resolved_to_radix"] is False and opt["resolved_kind"] == "pooled_bucket"


def test_round_disagreement_on_the_answer_is_an_error_not_a_timing() -> None:
    from atlas_api.engine import merge_lane_rounds

    lane_ = merge_lane_rounds(None, [_ok_run(10), _ok_run(11, distance=8)])
    assert lane_["status"] == "error" and "differed between rounds" in lane_["error"]["message"]


def test_all_rounds_failing_is_the_lanes_error() -> None:
    from atlas_api.engine import merge_lane_rounds

    bad = {"status": "error", "error": {"category": "engine", "message": "boom"}}
    lane_ = merge_lane_rounds(None, [bad, bad])
    assert lane_["status"] == "error" and lane_["error"]["message"] == "boom"
