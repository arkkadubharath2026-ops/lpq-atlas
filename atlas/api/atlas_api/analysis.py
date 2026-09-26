"""Aggregates of stored benchmark results (spec sections 14, 15, 34).

Reads lpq_benchmark's raw result files and summarises them with the project's
own statistics module: medians and MADs of per-query medians, and paired
comparisons against a chosen baseline with bootstrap intervals, a seeded
permutation test and Holm correction across the family. Effect sizes (median
ratio, rank-biserial, fraction of queries won) are always reported next to any
p-value, and no configuration is declared a winner: the fastest is identified
by its measured median and nothing else.
"""

from __future__ import annotations

import copy
import json
import statistics as pystats
from functools import lru_cache
from pathlib import Path
from typing import Any

from lpq_tools.statistics.comparison import adjust_comparisons, compare_paired

_SEED = 20260926


def _config_key(r: dict[str, Any]) -> str:
    return f"{r.get('algorithm')}/{r.get('pq')}/{r.get('mode')}"


def _mad(xs: list[float]) -> float:
    if not xs:
        return 0.0
    m = pystats.median(xs)
    return float(pystats.median(abs(x - m) for x in xs))


def summarise_file(
    path: Path, baseline_pq: str | None = None, *, compare: bool = True
) -> dict[str, Any]:
    """Summary of one result file; cached until the file changes."""
    return copy.deepcopy(_summarise(str(path), path.stat().st_mtime_ns, baseline_pq, compare))


@lru_cache(maxsize=64)
def _summarise(
    path_s: str, _mtime_ns: int, baseline_pq: str | None, compare: bool
) -> dict[str, Any]:
    path = Path(path_s)
    data = json.loads(path.read_text(encoding="utf-8"))
    records: list[dict[str, Any]] = data.get("records", [])
    by_cfg: dict[str, list[dict[str, Any]]] = {}
    for r in records:
        by_cfg.setdefault(_config_key(r), []).append(r)

    configs = []
    for key, rs in by_cfg.items():
        supported = [r for r in rs if r.get("supported", True)]
        first = rs[0]
        entry: dict[str, Any] = {
            "key": key,
            "algorithm": first.get("algorithm"),
            "pq": first.get("pq"),
            "mode": first.get("mode"),
            "queries": len(rs),
            "supported": bool(supported),
            "unsupported_reason": None if supported else first.get("unsupported_reason"),
        }
        if supported:
            meds = [float(r["timing"]["median_ns"]) for r in supported]
            ops = [r.get("pq_operations", {}) for r in supported]
            corr = [r.get("correctness", {}) for r in supported]
            ratios = [
                float(c["approximation_ratio"])
                for c in corr
                if c.get("approximation_ratio") is not None
            ]
            entry.update(
                median_of_medians_ns=float(pystats.median(meds)),
                mad_of_medians_ns=_mad(meds),
                per_query_median_ns=meds,
                within_query_noise=float(
                    pystats.median(float(r["timing"].get("relative_mad", 0.0)) for r in supported)
                ),
                total_comparisons=sum(int(o.get("comparisons", 0)) for o in ops),
                total_pops=sum(int(o.get("pops", 0)) for o in ops),
                total_settled=sum(int(o.get("settled_nodes", 0)) for o in ops),
                total_relaxations=sum(int(o.get("relaxations", 0)) for o in ops),
                exact_matches=sum(1 for c in corr if c.get("exact_match")),
                unreachable=sum(1 for c in corr if c.get("target_reachable") is False),
                max_approximation_ratio=max(ratios) if ratios else None,
                median_approximation_ratio=float(pystats.median(ratios)) if ratios else None,
                all_pinned=all(r["timing"].get("cpu_pinned", False) for r in supported),
                noisy_queries=sum(1 for r in supported if r["timing"].get("noisy")),
                queries_list=[[r.get("source"), r.get("target")] for r in supported],
            )
        configs.append(entry)

    measured = [c for c in configs if c["supported"]]
    fastest = min(measured, key=lambda c: c["median_of_medians_ns"])["key"] if measured else None

    comparisons: list[dict[str, Any]] = []
    base = None
    if measured and compare:
        if baseline_pq:
            base = next((c for c in measured if c["pq"] == baseline_pq), None)
        if base is None:
            base = next((c for c in measured if c["pq"] == "binary_heap"), measured[0])
        raw = []
        for c in measured:
            if c is base or c["queries_list"] != base["queries_list"]:
                continue
            raw.append(
                compare_paired(
                    c["per_query_median_ns"],
                    base["per_query_median_ns"],
                    seed=_SEED,
                    name_a=c["key"],
                    name_b=base["key"],
                )
            )
        for pc in adjust_comparisons(raw) if raw else ():
            comparisons.append(
                {
                    "config": pc.name_a,
                    "baseline": pc.name_b,
                    "pairs": pc.pairs,
                    "median_ratio_vs_baseline": pc.median_ratio,
                    "median_difference_ns": pc.median_difference,
                    "difference_ci_ns": [pc.difference_ci.low, pc.difference_ci.high],
                    "p_value": pc.p_value,
                    "adjusted_p_value": pc.adjusted_p_value,
                    "correction": pc.correction,
                    "test": pc.test,
                    "rank_biserial": pc.rank_biserial,
                    "fraction_baseline_faster": pc.fraction_b_faster,
                    "ties": pc.ties,
                }
            )
    for c in configs:
        c.pop("queries_list", None)
    env = records[0].get("environment", {}) if records else {}
    return {
        "file": path.name,
        "experiment_id": data.get("experiment_id"),
        "generated_at": data.get("generated_at"),
        "schema_version": data.get("schema_version"),
        "dataset": next((r.get("dataset") for r in records if r.get("dataset")), None),
        "graph_nodes": records[0].get("graph_nodes") if records else None,
        "records": len(records),
        "environment": env,
        "hardware_counters": records[0].get("hardware_counters") if records else None,
        "configurations": configs,
        "fastest_by_median": fastest,
        "baseline": base["key"] if base else None,
        "paired_comparisons": comparisons,
    }


def list_result_files(root: Path) -> list[Path]:
    raw = root / "results" / "raw"
    return sorted(p for p in raw.glob("*.json") if p.is_file())
