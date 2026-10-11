#!/usr/bin/env python3
"""Prepares the offline demonstration dataset ``demo_road``.

    python scripts/demo/prepare_demo.py            # needs a built lpq_convert
    python scripts/demo/prepare_demo.py --force    # reconvert even if current

What it does, in order, with no network access:

  1. copies the committed datasets/demo/demo_road.{gr,co}.gz into datasets/raw/
     (where the dataset pipeline expects raw files), verifying their digests
     against configs/datasets/demo_road.yaml;
  2. runs the repository's normal dataset pipeline for that config
     (lpq_tools.data.pipeline.prepare_dataset: validate, lpq_convert, metadata,
     checksum, query manifest) -- the same code path as every real dataset;
  3. compares the resulting graph checksum with the one recorded in the
     committed datasets/metadata/demo_road.json, and fails if they differ.

Step 2 calls the pipeline directly instead of scripts/data/prepare_datasets.py
because that CLI rewrites datasets/metadata/index.json from only the datasets it
was asked about, so preparing one dataset would replace the index of all of them.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from lpq_tools.data.pipeline import PipelineError, prepare_dataset  # noqa: E402
from lpq_tools.data.sources import load_dataset_config  # noqa: E402
from lpq_tools.utilities.hashing import sha256_file  # noqa: E402
from lpq_tools.utilities.jsonio import read_json  # noqa: E402
from lpq_tools.utilities.paths import RepoPaths  # noqa: E402

DATASET_ID = "demo_road"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--force", action="store_true", help="reconvert even if current")
    args = parser.parse_args(argv)

    paths = RepoPaths(ROOT)
    paths.ensure_directories()
    config = load_dataset_config(paths.dataset_configs / f"{DATASET_ID}.yaml")

    # Checksum recorded by whoever committed the metadata; read before the
    # converter overwrites it.
    metadata_path = paths.metadata / f"{DATASET_ID}.json"
    expected_checksum = None
    if metadata_path.exists():
        expected_checksum = read_json(metadata_path).get("graph_checksum_fnv1a64")

    for remote in config.files:
        source = ROOT / "datasets" / "demo" / remote.filename  # committed copy; the config's url is not used
        target = paths.raw / remote.filename
        if not source.is_file():
            print(f"error: {source} is missing; restore it with git, or regenerate with\n"
                  f"  python scripts/demo/make_demo_graph.py", file=sys.stderr)
            return 1
        if sha256_file(source) != remote.sha256:
            print(f"error: {source} does not match the digest pinned in "
                  f"configs/datasets/{DATASET_ID}.yaml (corrupted checkout, or a line-ending "
                  f"conversion of a binary file)", file=sys.stderr)
            return 1
        if target.exists():
            if sha256_file(target) != remote.sha256:
                print(f"error: {target} exists but differs from the committed file; raw "
                      f"files are immutable, delete it and re-run", file=sys.stderr)
                return 1
        else:
            shutil.copyfile(source, target)

    try:
        result = prepare_dataset(
            config, paths, force=args.force, progress=lambda m: print(f"  {m}", flush=True)
        )
    except (PipelineError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(result.summary())
    if expected_checksum is not None and int(expected_checksum) != result.header.checksum:
        print(
            f"error: the converted graph's checksum 0x{result.header.checksum:016x} differs "
            f"from the committed metadata's 0x{int(expected_checksum):016x}. The demo graph "
            "is not reproducing bit for bit on this machine; query results would not match "
            "the committed manifest.",
            file=sys.stderr,
        )
        return 1
    print(f"demo dataset ready: {result.graph_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
