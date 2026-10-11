#!/usr/bin/env python3
"""Summarises one lpq_benchmark results file (results/raw/local/*.json).

    python scripts/demo/summarize_results.py results/raw/local/<file>.json

Prints, per configuration: the median over queries of each query's median time,
and whether every answer matched the reference search. Then the provenance a
reader needs before trusting the numbers: CPU, compiler, commit, whether the
tree was dirty, whether the run was pinned to a core, how many measurements were
flagged noisy. Nothing is computed that the file does not contain; the numbers
are this machine's, not results.
"""

from __future__ import annotations

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    payload = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    records = payload["records"]

    by_label: dict[str, list[dict]] = defaultdict(list)
    for r in records:
        by_label[r.get("queue_config", {}).get("label") or r["pq"]].append(r)

    print(f"{argv[1]}\nexperiment {payload.get('experiment_id')}, {len(records)} records\n")
    print(f"{'configuration':<18}{'median ns':>14}{'queries':>9}{'answers = reference':>22}")
    for label, rs in by_label.items():
        ok = [r for r in rs if r.get("supported", True)]
        if not ok:
            print(f"{label:<18}{'not measured':>14}")
            continue
        med = statistics.median(r["timing"]["median_ns"] for r in ok)
        exact = sum(1 for r in ok if r["correctness"]["exact_match"])
        print(f"{label:<18}{med:>14,.0f}{len(ok):>9}{f'{exact}/{len(ok)}':>22}")

    first = records[0]
    env = first["environment"]
    timed = [r for r in records if r.get("supported", True)]
    pinned = sum(1 for r in timed if r["timing"]["cpu_pinned"])
    noisy = sum(1 for r in timed if r["timing"]["noisy"])
    quality = sum(1 for r in timed if r["timing"]["publication_quality"])
    print(f"\nCPU        {env['cpu_model']} ({env['logical_cores']} logical cores)")
    print(f"OS         {env['os']}")
    print(f"compiler   {env['compiler']} {env['compiler_version']}, {env['build_type']}, {env['optimization_flags']}")
    print(f"commit     {env['git_commit']}{' (dirty working tree)' if env['git_dirty'] else ''}")
    print(f"pinned     {pinned}/{len(timed)} measurements   noisy {noisy}/{len(timed)}   "
          f"publication_quality {quality}/{len(timed)}")
    print("HW counters  not collected" if not env.get("hardware_counters_available") else "HW counters  available")
    if any(not r["correctness"]["exact_match"] for r in timed):
        print("\nWARNING: some answers differ from the reference search; do not use these timings.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
