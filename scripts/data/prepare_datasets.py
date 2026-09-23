#!/usr/bin/env python3
"""Prepares datasets end to end (ESD.md section 26).

    python scripts/data/prepare_datasets.py                 # every config
    python scripts/data/prepare_datasets.py dimacs_ny       # one of them
    python scripts/data/prepare_datasets.py --list          # what is declared
    python scripts/data/prepare_datasets.py --skip-download # convert what is here

Adding a dataset is a new YAML file in configs/datasets/, not a change to this
script: nothing about any particular graph appears below.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PYTHON_ROOT = Path(__file__).resolve().parents[2] / "python"
if str(_PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYTHON_ROOT))

from lpq_tools.data.pipeline import prepare_all, summarise  # noqa: E402
from lpq_tools.data.sources import DatasetConfig, load_all_dataset_configs  # noqa: E402
from lpq_tools.utilities.jsonio import write_json  # noqa: E402
from lpq_tools.utilities.paths import RepoPaths  # noqa: E402


def _select(configs: list[DatasetConfig], wanted: list[str]) -> list[DatasetConfig]:
    if not wanted:
        return configs
    by_id = {c.dataset_id: c for c in configs}
    missing = [name for name in wanted if name not in by_id]
    if missing:
        raise SystemExit(
            "unknown dataset id(s): "
            + ", ".join(missing)
            + "\ndeclared: "
            + ", ".join(sorted(by_id))
        )
    return [by_id[name] for name in wanted]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("datasets", nargs="*", help="dataset ids; default is all of them")
    parser.add_argument("--list", action="store_true", help="list declared datasets and exit")
    parser.add_argument(
        "--force", action="store_true", help="re-download and re-convert even if current"
    )
    parser.add_argument(
        "--skip-download",
        action="store_true",
        help="use the raw files already present and never touch the network",
    )
    parser.add_argument("--quiet", action="store_true", help="only print the final summary")
    args = parser.parse_args(argv)

    paths = RepoPaths.discover()
    configs = load_all_dataset_configs(paths.dataset_configs)

    if args.list:
        for config in configs:
            coords = " +coordinates" if config.coordinate_file else ""
            weights = (
                f" weights={config.weights.distribution}(seed {config.weights.seed})"
                if config.weights
                else " weights=dataset"
            )
            print(f"{config.dataset_id:20s} {config.source_format:7s}{weights}{coords}")
        return 0

    selected = _select(configs, args.datasets)
    progress = None if args.quiet else (lambda message: print(f"  {message}", flush=True))

    results, failures = prepare_all(
        selected, paths, force=args.force, skip_download=args.skip_download, progress=progress
    )

    if results:
        print("\nPrepared:")
        for result in sorted(results, key=lambda r: r.dataset_id):
            print("  " + result.summary().replace("\n", "\n  "))
        index_path = paths.metadata / "index.json"
        write_json(index_path, summarise(results, paths.root))
        print(f"\nIndex written to {index_path}")

    if failures:
        print("\nFailed:", file=sys.stderr)
        for dataset_id, error in failures:
            print(f"  {dataset_id}: {error}", file=sys.stderr)
        # A partial run is a failure: an experiment that silently ran on four of
        # six datasets is worse than one that did not run.
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
