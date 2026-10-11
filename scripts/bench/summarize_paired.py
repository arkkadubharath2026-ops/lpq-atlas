#!/usr/bin/env python3
"""Paired summary of lpq_benchmark --interleave results, relative to a reference queue.

For each configuration: geometric mean of per-query ratios t_config / t_reference
with a seeded bootstrap 95% CI over queries, and the share of queries on which
the configuration was faster. Only queries where both are supported and exact are
used; nothing else is dropped.

    python scripts/bench/summarize_paired.py results/raw/policy/dimacs_ny_dijkstra.json [--ref radix_heap]
"""
import argparse
import json
import math
import random
from collections import defaultdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--ref", default="radix_heap")
    ap.add_argument("--seed", type=int, default=20260923)
    a = ap.parse_args()
    for path in a.files:
        recs = json.load(open(path))["records"]
        t = defaultdict(dict)  # label -> (source,target) -> median_ns
        for r in recs:
            if not r.get("supported", True):
                continue
            c = r.get("correctness", {})
            if c.get("exact_match") is False:
                raise SystemExit(f"{path}: {r['pq']} is not exact on a query; refusing to summarise")
            t[r["pq"]][(r["source"], r["target"])] = r["timing"]["median_ns"]
        ref = t[a.ref]
        ds = recs[0]["dataset"]
        print(f"\n{ds}  ({len(ref)} queries, reference {a.ref}, median {sorted(ref.values())[len(ref)//2]/1e6:.2f} ms)")
        print(f"  {'config':<22}{'geomean ratio':>14}{'95% CI':>20}{'faster on':>11}")
        for label, d in sorted(t.items(), key=lambda kv: 0 if kv[0] == a.ref else 1):
            keys = [k for k in d if k in ref]
            ratios = [d[k] / ref[k] for k in keys]
            g = math.exp(sum(map(math.log, ratios)) / len(ratios))
            rng = random.Random(a.seed)
            boots = sorted(
                math.exp(sum(math.log(rng.choice(ratios)) for _ in ratios) / len(ratios))
                for _ in range(2000)
            )
            lo, hi = boots[50], boots[1949]
            win = sum(r < 1 for r in ratios) / len(ratios)
            print(f"  {label:<22}{g:>14.3f}{f'[{lo:.3f},{hi:.3f}]':>20}{win:>10.0%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
