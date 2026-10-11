#!/usr/bin/env python3
"""Measure queue configurations on one or more graphs, paired and interleaved.

For each dataset this runs ONE lpq_benchmark process containing the strongest
classical queues (radix heap, binary heap), the pooled bucket queue at several
bucket widths with a ring sized to cover every key rise (the same rule the
engine's policy uses), and the replay floor (the search with queue work
removed). Configurations are interleaved per query in a seeded random order, so
slow host drift hits all of them equally.

Raw results go to results/raw/policy/<dataset>_<algorithm>.json and are never
overwritten (the benchmark binary refuses). Nothing is selected or excluded by
outcome: every configuration measured is in the file.

    python scripts/bench/run_policy_campaign.py --algorithm dijkstra dimacs_ny snap_facebook
"""
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIDTHS = (1, 2, 4, 8, 16)
MAX_POLICY_RING = 1 << 18


def pooled_ring_for(max_step: int, width: int) -> int:
    """Mirror of pooled_ring_for() in cpp/include/lpq/lpq/queue_policy.hpp."""
    need = max_step // width + 3
    return 1 << max(1, math.ceil(math.log2(need)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("datasets", nargs="+")
    ap.add_argument("--algorithm", choices=("dijkstra", "astar"), default="dijkstra")
    ap.add_argument("--heuristic", default="euclidean")
    ap.add_argument("--queries", type=int, default=30, help="use the first N queries of the manifest")
    ap.add_argument("--repetitions", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--cpu-core", type=int, default=2)
    ap.add_argument("--seed", type=int, default=20260923)
    ap.add_argument("--out-dir", default="results/raw/policy")
    ap.add_argument("--binary", default="build/release/bin/lpq_benchmark")
    ap.add_argument("--extra", default="", help="extra --compare entries, e.g. 'bucket_queue'")
    args = ap.parse_args()

    out_dir = ROOT / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    status = 0
    for ds in args.datasets:
        meta = json.loads((ROOT / f"datasets/metadata/{ds}.json").read_text())
        c = int(meta["max_weight"])
        step = 2 * c if args.algorithm == "astar" else c
        specs = ["radix_heap", "binary_heap"]
        for w in WIDTHS:
            ring = pooled_ring_for(step, w)
            if ring > MAX_POLICY_RING:
                print(f"{ds}: skip width {w} (ring {ring} > {MAX_POLICY_RING})")
                continue
            specs.append(f"pooled_bucket:w={w},ring={ring}@pooled_w{w}")
        specs.append("replay_floor")
        if args.extra:
            specs.append(args.extra)

        manifest = ROOT / f"experiments/manifests/{ds}_queries_first{args.queries}.yaml"
        if not manifest.exists():
            print(f"{ds}: missing {manifest.name}; run scripts/data/subset_manifest.py", file=sys.stderr)
            status = 1
            continue
        out = out_dir / f"{ds}_{args.algorithm}.json"
        cmd = [
            str(ROOT / args.binary),
            "--dataset", str(ROOT / f"datasets/processed/{ds}.lpqgraph"),
            "--queries-file", str(manifest),
            "--algorithm", args.algorithm,
            "--repetitions", str(args.repetitions),
            "--warmup", str(args.warmup),
            "--cpu-core", str(args.cpu_core),
            "--seed", str(args.seed),
            "--interleave",
            "--experiment-id", f"policy-campaign-{ds}-{args.algorithm}",
            "--output", str(out),
            "--compare", ";".join(specs),
        ]
        if args.algorithm == "astar":
            cmd += ["--heuristic", args.heuristic]
        print(f"== {ds} ({args.algorithm}): {len(specs)} configurations x {args.queries} queries", flush=True)
        r = subprocess.run(cmd, cwd=ROOT)
        if r.returncode != 0:
            print(f"{ds}: benchmark exited {r.returncode}", file=sys.stderr)
            status = r.returncode
    return status


if __name__ == "__main__":
    raise SystemExit(main())
