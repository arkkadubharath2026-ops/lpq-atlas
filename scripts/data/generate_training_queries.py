#!/usr/bin/env python3
"""Writes a training query manifest whose sources avoid every benchmark source.

    python scripts/data/generate_training_queries.py \\
        --benchmark experiments/manifests/snap_facebook_queries.yaml \\
        --also-exclude experiments/manifests/snap_facebook_queries_first100.yaml \\
        --seed 917 --count 200 \\
        --output experiments/manifests/snap_facebook_training_queries_source_disjoint.yaml

lpq_collect labels the training set from a full single-source search per query, so a training
query that shares a SOURCE with a benchmark query reuses that search even when the (source,
target) pair is different. This draws training sources only from the nodes no benchmark manifest
uses as a source. Targets are unconstrained: the labels do not depend on them.

The dataset id, node count and graph checksum are taken from --benchmark, and every
--also-exclude manifest must be for the same graph. The output records, in comments, which
files it excludes and their SHA-256, because the engine's reader rejects unknown keys.
Existing files are never overwritten.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PYTHON_ROOT = Path(__file__).resolve().parents[2] / "python"


def main(argv: list[str] | None = None) -> int:
    # Imported here, after the repository's python/ directory is on the path, rather than at module
    # level: that keeps the script runnable from any directory without import-order exceptions.
    if str(_PYTHON_ROOT) not in sys.path:
        sys.path.insert(0, str(_PYTHON_ROOT))
    from lpq_tools.data.queries import build_manifest, read_manifest, write_manifest
    from lpq_tools.utilities.hashing import sha256_file

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--benchmark", type=Path, required=True, help="the benchmark manifest")
    parser.add_argument("--also-exclude", type=Path, action="append", default=[],
                        help="further manifests whose sources are excluded (repeatable)")
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--count", type=int, required=True, help="number of training queries")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.output.exists():
        print(f"refusing to overwrite {args.output}", file=sys.stderr)
        return 1

    benchmark = read_manifest(args.benchmark)
    excluded: set[int] = {p.source for p in benchmark.pairs}
    notes = [
        "Source-disjoint training set: no source node is used by the benchmark manifest(s) below.",
        f"Seed {args.seed}; sources drawn uniformly from the nodes not used as a benchmark source.",
    ]
    inputs = [args.benchmark, *args.also_exclude]
    for path in inputs:
        other = read_manifest(path)
        if (other.dataset_id, other.node_count, other.graph_checksum) != (
            benchmark.dataset_id, benchmark.node_count, benchmark.graph_checksum
        ):
            print(f"{path} is for a different graph than {args.benchmark} "
                  "(dataset id, node count or graph checksum differ)", file=sys.stderr)
            return 1
        sources = {p.source for p in other.pairs}
        excluded |= sources
        notes.append(f"excludes sources of {path.name} (sha256 {sha256_file(path)}, "
                     f"{len(other.pairs)} queries, {len(sources)} distinct sources)")
    pool = benchmark.node_count - len(excluded)
    notes.append(f"{len(excluded)} sources excluded, {pool} allowed sources of {benchmark.node_count} nodes")

    try:
        manifest = build_manifest(
            benchmark.dataset_id,
            benchmark.node_count,
            count=args.count,
            seed=args.seed,
            graph_checksum=benchmark.graph_checksum,
            exclude_sources=excluded,
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    write_manifest(args.output, manifest, notes=notes)
    distinct = len({p.source for p in manifest.pairs})
    print(f"wrote {args.output}")
    print(f"  excluded sources : {len(excluded)} (from {len(inputs)} manifest(s))")
    print(f"  allowed sources  : {pool} of {benchmark.node_count} nodes")
    print(f"  queries          : {len(manifest.pairs)} ({distinct} distinct sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
