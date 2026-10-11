#!/usr/bin/env python3
"""Write the first N queries of a query manifest as a new manifest.

Deterministic (prefix of the original, same graph checksum and seed), so a
subset is as reproducible as the manifest it came from. Used to bound the cost
of multi-dataset runs without choosing queries by outcome.

    python scripts/data/subset_manifest.py experiments/manifests/dimacs_ny_queries_100.yaml 30
"""
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    n = int(sys.argv[2])
    text = src.read_text()
    head, _, body = text.partition("queries:\n")
    pairs = re.findall(r"- source: (\d+)\n  target: (\d+)\n", body)
    if len(pairs) < n:
        print(f"{src} has only {len(pairs)} queries", file=sys.stderr)
        return 1
    head = re.sub(r"query_count: \d+", f"query_count: {n}", head)
    out = src.with_name(re.sub(r"_\d+\.yaml$|\.yaml$", "", src.name) + f"_first{n}.yaml")
    if out.exists():
        print(f"refusing to overwrite {out}", file=sys.stderr)
        return 1
    out.write_text(head + "queries:\n" + "".join(f"- source: {s}\n  target: {t}\n" for s, t in pairs[:n]))
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
