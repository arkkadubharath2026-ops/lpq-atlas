"""Scoring a multi-lane comparison (pure functions, no engine, no I/O).

This is the single source of truth for every derived figure the comparison page
shows: speedup against the vanilla lane, ranking, the fastest lane, the
same-distance check, and the attribution of a difference to the algorithm, the
priority queue or the ML component. The web page only formats these numbers.

Nothing here measures anything. It reads the per-lane facts the engine reported
(median, spread, counters, distance) and does arithmetic on them.
"""

from __future__ import annotations

import statistics
from typing import Any

# A lane is "vanilla" when it pairs its algorithm with this queue.
VANILLA_QUEUE = "binary_heap"

# A difference counts as real only if it exceeds both a relative floor and a
# multiple of the two lanes' median absolute deviations. A single query is noisy,
# so anything inside this band is reported as "about the same", not as a win.
NOISE_FLOOR = 0.02
NOISE_MADS = 2.0

NOISE_RULE = (
    f"A difference is reported only if (1) it is larger than {NOISE_FLOOR:.0%} of the vanilla "
    f"median and larger than {NOISE_MADS:g}x the sum of the two lanes' spreads, where a lane's "
    "spread is the larger of its typical within-process deviation and the deviation of its "
    "per-round medians, and (2) the same lane was ahead in a strict majority of the measurement "
    "rounds. Otherwise the lanes are reported as about the same. One round is never enough to "
    "call a winner."
)


def median(xs: list[float]) -> float:
    return float(statistics.median(xs))


def mad(xs: list[float]) -> float:
    """Median absolute deviation about the median; 0 for fewer than two values."""
    if len(xs) < 2:
        return 0.0
    m = median(xs)
    return median([abs(x - m) for x in xs])


def aggregate_rounds(rounds: list[dict[str, Any] | None]) -> dict[str, Any] | None:
    """Combine one lane's per-round engine timings into the lane's timing record.

    `rounds` has one entry per round, in round order: the engine's benchmark block for
    that round, or None if the lane failed in it. The lane's median is the median of the
    per-round medians. Its spread is the LARGER of the median within-process MAD and the
    MAD of the per-round medians: within-process MAD alone understates the drift between
    separate processes on a shared machine, so the between-round figure is never dropped.
    Returns None when no round produced a timing.
    """
    ok = [r for r in rounds if r]
    if not ok:
        return None
    medians = [float(r["median_ns"]) for r in ok]
    within = median([float(r["mad_ns"]) for r in ok])
    between = mad(medians)
    med = median(medians)
    samples = [x for r in ok for x in r.get("samples_ns", [])]
    warnings: list[str] = []
    for r in ok:
        for w in r.get("warnings", []):
            if w not in warnings:
                warnings.append(w)
    spread = max(within, between)
    return {
        "median_ns": med,
        "mad_ns": spread,
        "mad_within_ns": within,
        "mad_between_rounds_ns": between,
        "round_medians_ns": [float(r["median_ns"]) if r else None for r in rounds],
        "rounds": len(rounds),
        "rounds_ok": len(ok),
        "relative_mad": spread / med if med > 0 else None,
        "min_ns": min((r["min_ns"] for r in ok if r.get("min_ns") is not None), default=None),
        "max_ns": max((r["max_ns"] for r in ok if r.get("max_ns") is not None), default=None),
        "samples_ns": samples,
        "repetitions": ok[0].get("repetitions"),
        "warmup": ok[0].get("warmup"),
        "noisy": any(bool(r.get("noisy")) for r in ok),
        "pinned": all(bool(r.get("pinned")) for r in ok),
        "core": ok[0].get("core"),
        "warnings": warnings,
    }


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
    """How `lane` compares with `base`: speedup = base median / lane median.

    Besides the noise rule, a win or a loss needs the same sign in a strict majority of
    the rounds in which both lanes have a timing (and at least two such rounds).
    """
    lt, bt = lane["timing"], base["timing"]
    s = speedup(bt["median_ns"], lt["median_ns"])
    out: dict[str, Any] = {
        "speedup": s,
        "verdict": "unavailable",
        "within_noise": None,
        "reason": None,
        "rounds_faster": 0,
        "rounds_slower": 0,
        "rounds_compared": 0,
    }
    if s is None:
        return out
    pairs = [
        (b, x)
        for b, x in zip(bt.get("round_medians_ns") or [], lt.get("round_medians_ns") or [], strict=False)
        if b and x
    ]
    n = len(pairs)
    faster = sum(1 for b, x in pairs if b / x > 1.0)
    slower = sum(1 for b, x in pairs if b / x < 1.0)
    out.update(rounds_faster=faster, rounds_slower=slower, rounds_compared=n)
    noise = _within_noise(
        lt["median_ns"], lt.get("mad_ns") or 0.0, bt["median_ns"], bt.get("mad_ns") or 0.0
    )
    out["within_noise"] = noise
    if noise:
        out.update(verdict="same", reason="within_noise")
    elif n < 2:
        out.update(verdict="same", reason="single_round")
    elif s > 1.0 and faster * 2 > n:
        out.update(verdict="faster")
    elif s < 1.0 and slower * 2 > n:
        out.update(verdict="slower")
    else:
        out.update(verdict="same", reason="not_consistent_across_rounds")
    return out


def effect_kind(lane: dict[str, Any]) -> str:
    """What changed relative to the vanilla lane of the same algorithm."""
    if lane["queue"] == VANILLA_QUEUE:
        return "vanilla"
    if lane.get("family") == "learned":
        # An Optimized lane that resolved to the radix heap IS the radix heap: whatever it
        # gained over vanilla is the queue choice, and must not be credited to ML.
        return "queue" if lane.get("resolved_kind") == "radix_heap" else "ml"
    return "queue"


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
        lane["verdict_reason"] = None
        lane["rounds_faster"] = lane["rounds_slower"] = lane["rounds_compared"] = None
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
                speedup_vs_vanilla=c["speedup"],
                verdict=c["verdict"],
                within_noise=c["within_noise"],
                verdict_reason=c["reason"],
                rounds_faster=c["rounds_faster"],
                rounds_slower=c["rounds_slower"],
                rounds_compared=c["rounds_compared"],
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
        # Tied: not demonstrably slower than the leader (noise rule AND round majority).
        tied = [o["id"] for o in placed[1:] if _compare(o, top)["verdict"] != "slower"]
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
                "reason": c["reason"],
                "rounds_faster": c["rounds_faster"],
                "rounds_slower": c["rounds_slower"],
                "rounds_compared": c["rounds_compared"],
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
    # Optimized against the strongest non-ML queue of the same algorithm: the radix heap when
    # that lane is present, else the fastest other classical queue. This, not vanilla, is the
    # fair test of whether the Optimized queue earns its place.
    for lane in lanes:
        if not _timing_ok(lane) or lane.get("family") != "learned":
            continue
        classical = [
            o
            for o in lanes
            if o is not lane
            and o["algorithm"] == lane["algorithm"]
            and o.get("family") == "classical"
            and o["queue"] != VANILLA_QUEUE
            and _timing_ok(o)
        ]
        if not classical:
            continue
        radix = [o for o in classical if o["queue"] == "radix_heap"]
        best = radix[0] if radix else min(classical, key=lambda o: o["timing"]["median_ns"])
        add("ml_vs_best_classical", lane, best)

    return {
        "fastest": fastest,
        "correctness": correctness,
        "effects": effects,
        "noise_rule": NOISE_RULE,
        "vanilla_queue": VANILLA_QUEUE,
    }
