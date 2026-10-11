"""Scoring a multi-lane comparison (pure functions, no engine, no I/O).

This is the single source of truth for every derived figure the comparison page
shows: speedup against the vanilla lane, ranking, the fastest lane, the
same-distance check, and the attribution of a difference to the algorithm, the
priority queue or the ML component. The web page only formats these numbers.

Nothing here measures anything. It reads the per-lane facts the engine reported
(median, spread, counters, distance) and does arithmetic on them.
"""

from __future__ import annotations

from typing import Any

# A lane is "vanilla" when it pairs its algorithm with this queue.
VANILLA_QUEUE = "binary_heap"

# A difference counts as real only if it exceeds both a relative floor and a
# multiple of the two lanes' median absolute deviations. A single query is noisy,
# so anything inside this band is reported as "about the same", not as a win.
NOISE_FLOOR = 0.02
NOISE_MADS = 2.0

NOISE_RULE = (
    f"A difference is reported only if it is larger than {NOISE_FLOOR:.0%} of the vanilla "
    f"median and larger than {NOISE_MADS:g}x the sum of the two lanes' median absolute "
    "deviations; otherwise the lanes are reported as about the same."
)


def _within_noise(a_med: float, a_mad: float, b_med: float, b_mad: float) -> bool:
    delta = abs(a_med - b_med)
    return delta <= max(NOISE_MADS * (a_mad + b_mad), NOISE_FLOOR * max(a_med, b_med))


def speedup(vanilla_median_ns: float, lane_median_ns: float) -> float | None:
    """vanilla_median / lane_median: above 1 means the lane is faster."""
    if vanilla_median_ns <= 0 or lane_median_ns <= 0:
        return None
    return vanilla_median_ns / lane_median_ns


def _timing_ok(lane: dict[str, Any]) -> bool:
    return lane.get("status") == "ok" and bool(lane.get("timing", {}).get("median_ns"))


def _compare(lane: dict[str, Any], base: dict[str, Any]) -> dict[str, Any]:
    """How `lane` compares with `base`: speedup = base median / lane median."""
    lt, bt = lane["timing"], base["timing"]
    s = speedup(bt["median_ns"], lt["median_ns"])
    if s is None:
        return {"speedup": None, "verdict": "unavailable", "within_noise": None}
    noise = _within_noise(
        lt["median_ns"], lt.get("mad_ns") or 0.0, bt["median_ns"], bt.get("mad_ns") or 0.0
    )
    verdict = "same" if noise else ("faster" if s > 1.0 else "slower")
    return {"speedup": s, "verdict": verdict, "within_noise": noise}


def effect_kind(lane: dict[str, Any]) -> str:
    """What changed relative to the vanilla lane of the same algorithm."""
    if lane["queue"] == VANILLA_QUEUE:
        return "vanilla"
    return "ml" if lane.get("family") == "learned" else "queue"


def analyse(
    lanes: list[dict[str, Any]], reference: dict[str, Any] | None = None
) -> dict[str, Any]:
    """Annotate lanes in place and return the cross-lane summary.

    Each lane needs: id, algorithm, queue, family, status ("ok" or "error"), and for
    ok lanes `timing.median_ns`, `timing.mad_ns`, `distance`, `reachable`, and
    `reference` ({reachable, distance}) as the engine's own reference search found.
    """
    ok = [lane for lane in lanes if lane.get("status") == "ok"]

    # -- same-distance check ------------------------------------------------
    refs = {(lane["reference"]["reachable"], lane["reference"]["distance"]) for lane in ok}
    ref = reference or (
        {"reachable": next(iter(refs))[0], "distance": next(iter(refs))[1]} if refs else None
    )
    for lane in lanes:
        if lane.get("status") != "ok" or ref is None:
            lane["matches_reference"] = None
            continue
        lane["matches_reference"] = (
            lane["reachable"] == ref["reachable"] and lane["distance"] == ref["distance"]
        )
    mismatches = [lane["id"] for lane in ok if lane["matches_reference"] is False]
    distances = {(lane["reachable"], lane["distance"]) for lane in ok}
    correctness = {
        "checked_lanes": len(ok),
        "failed_lanes": [lane["id"] for lane in lanes if lane.get("status") != "ok"],
        "references_agree": len(refs) <= 1,
        "all_lanes_same_distance": len(distances) <= 1 if ok else None,
        "all_match_reference": (not mismatches and len(refs) <= 1) if ok else None,
        "mismatching_lanes": mismatches,
        "reference_distance": ref["distance"] if ref else None,
        "reachable": ref["reachable"] if ref else None,
    }

    # -- speedup against the vanilla lane of the same algorithm -----------------
    vanilla_by_alg = {
        lane["algorithm"]: lane
        for lane in lanes
        if lane["queue"] == VANILLA_QUEUE and _timing_ok(lane)
    }
    for lane in lanes:
        lane["effect"] = effect_kind(lane)
        lane["vanilla_lane_id"] = None
        lane["speedup_vs_vanilla"] = None
        lane["verdict"] = "unavailable"
        lane["within_noise"] = None
        if not _timing_ok(lane):
            continue
        vanilla = vanilla_by_alg.get(lane["algorithm"])
        if vanilla is None:
            continue
        lane["vanilla_lane_id"] = vanilla["id"]
        if vanilla is lane:
            lane.update(speedup_vs_vanilla=1.0, verdict="reference", within_noise=None)
        else:
            c = _compare(lane, vanilla)
            lane.update(
                speedup_vs_vanilla=c["speedup"], verdict=c["verdict"], within_noise=c["within_noise"]
            )

    # -- ranking: only lanes with a timing AND the right answer can place -----------
    placed = sorted(
        (lane for lane in ok if _timing_ok(lane) and lane["matches_reference"] is not False),
        key=lambda lane: lane["timing"]["median_ns"],
    )
    for lane in lanes:
        lane["rank"] = None
    for i, lane in enumerate(placed, start=1):
        lane["rank"] = i
        lane["vs_fastest"] = speedup(placed[0]["timing"]["median_ns"], lane["timing"]["median_ns"])
    for lane in lanes:
        lane.setdefault("vs_fastest", None)

    fastest = None
    if placed:
        top = placed[0]
        tied = [
            other["id"]
            for other in placed[1:]
            if _within_noise(
                top["timing"]["median_ns"],
                top["timing"].get("mad_ns") or 0.0,
                other["timing"]["median_ns"],
                other["timing"].get("mad_ns") or 0.0,
            )
        ]
        fastest = {
            "lane_id": top["id"],
            "median_ns": top["timing"]["median_ns"],
            "tied_with": tied,
            "clear_win": not tied,
        }

    # -- attribution: algorithm vs queue vs ML --------------------------------
    effects: list[dict[str, Any]] = []
    by_id = {lane["id"]: lane for lane in lanes}

    def add(kind: str, lane: dict[str, Any], base: dict[str, Any]) -> None:
        c = _compare(lane, base)
        effects.append(
            {
                "kind": kind,
                "lane_id": lane["id"],
                "versus_lane_id": base["id"],
                "speedup": c["speedup"],
                "verdict": c["verdict"],
            }
        )

    # Algorithm: the vanilla A* lane against the vanilla Dijkstra lane (same queue).
    vd, va = vanilla_by_alg.get("dijkstra"), vanilla_by_alg.get("astar")
    if vd and va:
        add("algorithm", va, vd)
    for lane in lanes:
        if not _timing_ok(lane) or lane["effect"] == "vanilla":
            continue
        vid = lane["vanilla_lane_id"]
        if vid is not None:
            add(lane["effect"], lane, by_id[vid])
    # ML against the best classical queue of the same algorithm, when that is not
    # simply the vanilla queue: the fair question once a faster classical queue exists.
    for lane in lanes:
        if not _timing_ok(lane) or lane["effect"] != "ml":
            continue
        classical = [
            o
            for o in lanes
            if o is not lane
            and o["algorithm"] == lane["algorithm"]
            and o.get("family") == "classical"
            and _timing_ok(o)
        ]
        if not classical:
            continue
        best = min(classical, key=lambda o: o["timing"]["median_ns"])
        if best["queue"] != VANILLA_QUEUE:
            add("ml_vs_best_classical", lane, best)

    return {
        "fastest": fastest,
        "correctness": correctness,
        "effects": effects,
        "noise_rule": NOISE_RULE,
        "vanilla_queue": VANILLA_QUEUE,
    }
