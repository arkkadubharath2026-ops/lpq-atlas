"""Shared builders for the policy tests (no tests in this module).

The synthetic world has a KNOWN truth: for each graph the true log-ratio of pooled
width ``w`` to radix is linear in ``p50_over_max`` only, so radix wins when that
ratio is high and the pooled queue wins when it is low.  Result files are built
with the same schema ``lpq_benchmark --interleave`` writes.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
from lpq_tools.policy.profile import WeightProfile, format_profile

#: truth: log(t_pooled_w / t_radix) = A[w] + B[w] * p50_over_max
TRUTH = {1: (-0.5, 2.0), 8: (-0.3, 1.2)}


def true_log_ratio(width: int, p50_over_max: float) -> float:
    a, b = TRUTH[width]
    return a + b * p50_over_max


def make_profile(
    nodes: int, arcs: int, max_w: float, mean_w: float, p50_ratio: float, p99_ratio: float = 0.8,
    cv: float = 0.5, checksum: int = 1,
) -> WeightProfile:
    return WeightProfile(
        nodes=nodes, arcs=arcs, min_weight=1.0, max_weight=max_w, mean_weight=mean_w,
        stddev_weight=cv * mean_w, p50=p50_ratio * max_w, p90=0.9 * max_w, p99=p99_ratio * max_w,
        mean_degree=arcs / nodes, checksum=checksum)


def write_graph_files(
    root: Path, dataset: str, profile: WeightProfile, derived_from: str | None = None,
    extra_meta: dict | None = None,
) -> None:
    (root / "profiles").mkdir(exist_ok=True)
    (root / "meta").mkdir(exist_ok=True)
    (root / "profiles" / f"{dataset}.lpqprofile").write_text(format_profile(profile))
    meta: dict = {"dataset_id": dataset, "graph_checksum_fnv1a64": profile.checksum,
                  "node_count": profile.nodes, "arc_count": profile.arcs}
    if derived_from:
        meta["derived_from"] = derived_from
    meta.update(extra_meta or {})
    (root / "meta" / f"{dataset}.json").write_text(json.dumps(meta))


def record(
    dataset: str, algorithm: str, label: str, queue: str, width: int, src: int, dst: int,
    t_ns: float, nodes: int = 1000, edges: int = 3000, supported: bool = True,
    exact: bool | None = True, ring: int = 0,
) -> dict:
    rec = {
        "schema_version": 1, "dataset": dataset, "algorithm": algorithm, "pq": label,
        "source": src, "target": dst, "supported": supported, "graph_nodes": nodes,
        "graph_edges": edges,
        "queue_config": {"label": label, "queue": queue, "bucket_width": width,
                         "ring_size": ring, "front": -1, "strategy": "", "order_position": 0},
        "timing": {"median_ns": t_ns},
    }
    if exact is not None:
        rec["correctness"] = {"exact_match": exact}
    return rec


def write_results(path: Path, records: list[dict], experiment_id: str = "synthetic") -> Path:
    path.write_text(json.dumps({"schema_version": 1, "experiment_id": experiment_id,
                                "generated_at": "2026-01-01T00:00:00Z", "records": records}))
    return path


def synth_records(
    dataset: str, truth: dict[int, float], n_queries: int, rng: np.random.Generator,
    algorithm: str = "dijkstra", nodes: int = 1000, edges: int = 3000, noise: float = 0.02,
) -> list[dict]:
    """Interleaved-style records: for each query, radix + one pooled row per width."""
    recs = []
    for q in range(n_queries):
        t_r = float(math.exp(rng.normal(math.log(2e5), 0.3)))
        recs.append(record(dataset, algorithm, "radix_heap", "radix_heap", 0, q, q + 1, t_r,
                           nodes, edges))
        for w, z in truth.items():
            t = t_r * math.exp(z + rng.normal(0.0, noise))
            recs.append(record(dataset, algorithm, f"pooled_w{w}", "pooled_bucket", w, q, q + 1,
                               t, nodes, edges, ring=8192))
        recs.append(record(dataset, algorithm, "replay_floor", "replay_floor", 0, q, q + 1,
                           t_r * 0.5, nodes, edges))
    return recs


def build_world(
    root: Path, seed: int = 7, n_topologies: int = 6, derived_per_topology: int = 1,
    n_queries: int = 20, widths: tuple[int, ...] = (1, 8),
) -> tuple[list[Path], dict[str, float]]:
    """Writes profiles, metadata and one result file per graph.

    Returns the result file paths and ``{dataset: p50_over_max}``.  Each topology has a
    base graph and ``derived_per_topology`` derived graphs (same node/arc counts,
    different weights).  Topology ``t`` has ``nodes = 1000 * (t + 1)`` so topologies are
    distinguishable by shape too.
    """
    rng = np.random.default_rng(seed)
    root.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    ratios: dict[str, float] = {}
    checksum = 100
    for t in range(n_topologies):
        nodes, arcs = 1000 * (t + 1), 3000 * (t + 1) + 7
        base = f"base_{t}"
        for k in range(derived_per_topology + 1):
            ds = base if k == 0 else f"derived_{t}_{k}"
            r = float(rng.uniform(0.02, 0.5))
            checksum += 1
            prof = make_profile(nodes, arcs, max_w=float(rng.integers(500, 5000)),
                                mean_w=float(rng.uniform(5, 100)), p50_ratio=r,
                                p99_ratio=float(rng.uniform(0.3, 1.0)), cv=float(rng.uniform(0.2, 2)),
                                checksum=checksum)
            write_graph_files(root, ds, prof, derived_from=None if k == 0 else base)
            truth = {w: true_log_ratio(w, r) for w in widths}
            paths.append(write_results(root / f"{ds}.json",
                                       synth_records(ds, truth, n_queries, rng, nodes=nodes,
                                                     edges=arcs),
                                       experiment_id=f"synthetic-{ds}"))
            ratios[ds] = r
    return paths, ratios
