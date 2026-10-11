"""Derived datasets: same topology as a real graph, different edge weights.

A learned queue-configuration policy trained and validated on six real graphs
has six points on the axis it most needs to generalise over -- the weight
distribution. ``lpq_reweight`` (``cpp/apps/lpq_reweight.cpp``) rebuilds a real
graph with the topology, coordinates and node numbering left exactly as they
were and only the weights replaced; this module wraps it so that the result is a
first-class dataset: a processed graph, a metadata record, and query manifests
whose endpoints are the base graph's endpoints verbatim.

-------------------------------------------------------------------------------
Derived graphs are NOT real networks
-------------------------------------------------------------------------------

Every artifact written here says so. The dataset id must start with
``derived_``; the metadata carries ``weights.synthesized: true`` together with
``derived_from``, ``transform`` and ``seed``; and the description says the
weights are synthetic. A results table that attributes a derived graph's
timings to the road network it was derived from is wrong, and the labelling
exists so that mistake is visible rather than silent.

-------------------------------------------------------------------------------
Determinism
-------------------------------------------------------------------------------

The weights are a pure function of (base graph, transform spec, seed). The
integer-only transforms (``unit``, ``uniform``, ``scale``, ``clip``,
``bimodal``) are exact on any platform. ``lognormal`` and ``pareto`` evaluate
``exp``/``log``/``pow``/``sin``/``cos`` from the C library, and a different libm
version may round the last bit differently, so for those two the pinned
checksum in ``configs/derived_datasets.yaml`` is the checksum produced on the
generating host, not a cross-platform guarantee. :func:`derive_all` reports a
mismatch rather than overwriting anything.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import yaml

from lpq_tools.data.binary_header import BinaryGraphHeader, read_binary_graph_header
from lpq_tools.data.pipeline import _relative_to
from lpq_tools.data.queries import QueryManifest, read_manifest, write_manifest
from lpq_tools.utilities.jsonio import read_json, write_json
from lpq_tools.utilities.paths import RepoPaths

#: Largest weight ``lpq_reweight`` will emit (kMaxWeight in the C++ source).
MAX_WEIGHT = 0x7FFFFFFF

#: Manifest kinds copied from the base dataset when the base has them. The pairs
#: are a function of node count and seed only, and topology is unchanged, so the
#: base's endpoints are reused verbatim and the queries stay comparable.
MANIFEST_SUFFIXES = ("queries", "queries_first30", "training_queries")

_ID_PATTERN = re.compile(r"^derived_[a-z0-9][a-z0-9_]*$")

#: Number of numeric parameters per transform family.
_ARITY = {
    "unit": 0,
    "uniform": 2,
    "scale": 1,
    "clip": 1,
    "lognormal": 2,
    "pareto": 2,
    "bimodal": 3,
}

_PARAM_NAMES = {
    "unit": (),
    "uniform": ("minimum", "maximum"),
    "scale": ("factor",),
    "clip": ("cap",),
    "lognormal": ("median", "sigma"),
    "pareto": ("xmin", "alpha"),
    "bimodal": ("small", "large", "p_large"),
}

DESCRIPTION_NOTE = (
    "DERIVED DATASET WITH SYNTHETIC WEIGHTS. Topology, node numbering and "
    "coordinates are those of {base}; every edge weight was replaced by "
    "`lpq_reweight --transform {transform} --seed {seed}`. This is not a real "
    "road network and its timings must not be attributed to {base}."
)


class DeriveError(RuntimeError):
    """Raised when a derived dataset cannot be produced or verified."""


@dataclass(frozen=True)
class Transform:
    family: str
    params: tuple[float, ...]
    spec: str

    def parameters(self) -> dict[str, float]:
        return dict(zip(_PARAM_NAMES[self.family], self.params))


def parse_transform(spec: str) -> Transform:
    """Splits a transform spec into family and numeric parameters.

    Only the shape is checked here. Range validation belongs to
    ``lpq_reweight``, which is authoritative and reports its own error.
    """
    parts = spec.split(":")
    family = parts[0]
    if family not in _ARITY:
        raise DeriveError(
            f"unknown transform family '{family}' (expected one of {', '.join(_ARITY)})"
        )
    if len(parts) - 1 != _ARITY[family]:
        raise DeriveError(f"transform '{family}' takes {_ARITY[family]} parameter(s): '{spec}'")
    try:
        params = tuple(float(p) for p in parts[1:])
    except ValueError as error:
        raise DeriveError(f"non-numeric transform parameter in '{spec}'") from error
    return Transform(family=family, params=params, spec=spec)


@dataclass
class DerivedResult:
    dataset_id: str
    base_id: str
    transform: str
    seed: int
    graph_path: Path
    metadata_path: Path
    config_path: Path
    manifest_paths: list[Path]
    header: BinaryGraphHeader
    metadata: dict[str, Any]
    reweight_summary: str = ""

    @property
    def checksum(self) -> int:
        return self.header.checksum


def _tool(paths: RepoPaths) -> Path:
    try:
        return paths.tool("lpq_reweight")
    except FileNotFoundError as error:
        raise DeriveError(str(error)) from error


def derived_config_path(paths: RepoPaths, dataset_id: str) -> Path:
    return paths.root / "configs" / "datasets" / "derived" / f"{dataset_id}.yaml"


def _manifest_path(paths: RepoPaths, dataset_id: str, suffix: str) -> Path:
    return paths.manifests / f"{dataset_id}_{suffix}.yaml"


def _build_metadata(
    *,
    paths: RepoPaths,
    base_meta: dict[str, Any],
    stats: dict[str, Any],
    base_id: str,
    new_id: str,
    transform: Transform,
    seed: int,
    graph_path: Path,
    base_graph: Path,
) -> dict[str, Any]:
    """Assembles a record in the schema ``lpq_convert`` writes, plus provenance."""
    meta: dict[str, Any] = {
        "dataset_id": new_id,
        "source_path": _relative_to(paths.root, base_graph),
        "source_format": "binary",
        "processed_path": _relative_to(paths.root, graph_path),
    }
    for key in (
        "graph_checksum_fnv1a64",
        "weight_type",
        "node_count",
        "edge_count",
        "arc_count",
        "directed",
        "density",
        "min_weight",
        "max_weight",
        "mean_weight",
        "stddev_weight",
        "max_path_cost_bound",
        "min_out_degree",
        "max_out_degree",
        "mean_out_degree",
        "stddev_out_degree",
        "isolated_nodes",
        "has_coordinates",
        "has_node_id_mapping",
    ):
        meta[key] = stats[key]
    meta["normalization"] = base_meta.get("normalization", {})

    meta["derived"] = True
    meta["derived_from"] = base_id
    meta["base_graph_checksum_fnv1a64"] = base_meta.get("graph_checksum_fnv1a64")
    meta["transform"] = transform.spec
    meta["seed"] = seed
    meta["description"] = DESCRIPTION_NOTE.format(
        base=base_id, transform=transform.spec, seed=seed
    )
    meta["weights"] = {
        "synthesized": True,
        "derived_from": base_id,
        "transform": transform.spec,
        "seed": seed,
        "distribution": transform.family,
        "parameters": transform.parameters(),
        "clamped_low_draws": stats["clamped_low_draws"],
        "clamped_high_draws": stats["clamped_high_draws"],
        "tool": "lpq_reweight",
    }
    return meta


def _write_dataset_config(path: Path, meta: dict[str, Any]) -> None:
    """The committed, human-readable record of what a derived dataset is."""
    weights = meta["weights"]
    body = {
        "dataset_id": meta["dataset_id"],
        "derived": True,
        "description": meta["description"],
        "derived_from": meta["derived_from"],
        "transform": meta["transform"],
        "seed": meta["seed"],
        "weights": {
            "synthesized": True,
            "distribution": weights["distribution"],
            "parameters": weights["parameters"],
        },
        "graph_checksum_fnv1a64": meta["graph_checksum_fnv1a64"],
        "base_graph_checksum_fnv1a64": meta["base_graph_checksum_fnv1a64"],
        "metadata": f"datasets/metadata/{meta['dataset_id']}.json",
        "processed_graph": meta["processed_path"],
    }
    header = (
        "# DERIVED dataset -- synthetic weights, NOT a real road network.\n"
        "#\n"
        "# Generated by lpq_tools.data.derive; regenerate from\n"
        "# configs/derived_datasets.yaml rather than editing. This directory is\n"
        "# deliberately separate from configs/datasets/*.yaml, which describes real\n"
        "# downloadable datasets and is loaded as such.\n"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(header)
        yaml.safe_dump(body, handle, sort_keys=False, default_flow_style=False)


def _derive_manifests(
    paths: RepoPaths, base_id: str, new_id: str, header: BinaryGraphHeader
) -> list[Path]:
    written: list[Path] = []
    for suffix in MANIFEST_SUFFIXES:
        source = _manifest_path(paths, base_id, suffix)
        if not source.exists():
            continue
        base_manifest = read_manifest(source)
        if base_manifest.node_count != header.node_count:
            raise DeriveError(
                f"{source.name} has node_count {base_manifest.node_count} but the derived "
                f"graph has {header.node_count}; topology was supposed to be unchanged"
            )
        derived = QueryManifest(
            dataset_id=new_id,
            node_count=base_manifest.node_count,
            seed=base_manifest.seed,
            pairs=base_manifest.pairs,
            graph_checksum=header.checksum,
        )
        target = _manifest_path(paths, new_id, suffix)
        write_manifest(target, derived)
        written.append(target)
    return written


def derive_dataset(
    base_id: str,
    transform: str,
    seed: int,
    new_id: str,
    *,
    paths: RepoPaths | None = None,
    progress: Callable[[str], None] | None = None,
) -> DerivedResult:
    """Derives ``new_id`` from ``base_id`` by reweighting; see the module docstring.

    Refuses to overwrite: if the graph, metadata, config or any manifest already
    exists nothing is touched. Anything this call created is removed again if a
    later step fails, so a failure never leaves a graph without its metadata.
    """
    paths = paths or RepoPaths.discover()

    def say(message: str) -> None:
        if progress is not None:
            progress(message)

    if not _ID_PATTERN.match(new_id):
        raise DeriveError(
            f"derived dataset ids must match {_ID_PATTERN.pattern}; got '{new_id}'. "
            "The derived_ prefix is what keeps a synthetic-weight graph from being "
            "mistaken for a real one."
        )
    if base_id.startswith("derived_"):
        raise DeriveError("refusing to derive from a derived dataset; derive from the real base")
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        raise DeriveError(f"seed must be a non-negative integer, got {seed!r}")
    parsed = parse_transform(transform)

    base_graph = paths.processed / f"{base_id}.lpqgraph"
    base_meta_path = paths.metadata / f"{base_id}.json"
    if not base_graph.exists():
        raise DeriveError(f"base graph {base_graph} does not exist")
    if not base_meta_path.exists():
        raise DeriveError(f"base metadata {base_meta_path} does not exist")
    base_meta = read_json(base_meta_path)
    base_header = read_binary_graph_header(base_graph)
    if int(base_meta.get("graph_checksum_fnv1a64", -1)) != base_header.checksum:
        raise DeriveError(f"{base_id}: metadata checksum disagrees with the graph file")
    if base_header.weight_type != "uint32":
        raise DeriveError(f"{base_id}: only uint32 graphs can be reweighted")

    graph_path = paths.processed / f"{new_id}.lpqgraph"
    metadata_path = paths.metadata / f"{new_id}.json"
    config_path = derived_config_path(paths, new_id)
    manifest_targets = [_manifest_path(paths, new_id, s) for s in MANIFEST_SUFFIXES]
    for existing in (graph_path, metadata_path, config_path, *manifest_targets):
        if existing.exists():
            raise DeriveError(f"refusing to overwrite existing {existing}")

    tool = _tool(paths)
    created: list[Path] = []
    try:
        paths.processed.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="lpq_reweight_") as scratch:
            stats_path = Path(scratch) / "stats.json"
            say(f"{new_id}: reweighting {base_id} with {transform} (seed {seed})")
            completed = subprocess.run(
                [
                    str(tool),
                    "--input", str(base_graph),
                    "--output", str(graph_path),
                    "--transform", transform,
                    "--seed", str(seed),
                    "--stats-json", str(stats_path),
                ],
                capture_output=True,
                text=True,
                check=False,
                cwd=str(paths.root),
            )
            if completed.returncode != 0:
                raise DeriveError(
                    f"lpq_reweight failed (exit {completed.returncode}): "
                    f"{completed.stderr.strip()}"
                )
            created.append(graph_path)
            stats = read_json(stats_path)

        header = read_binary_graph_header(graph_path)
        if int(stats["graph_checksum_fnv1a64"]) != header.checksum:
            raise DeriveError("lpq_reweight's stats checksum disagrees with the file it wrote")
        if header.node_count != base_header.node_count or header.arc_count != base_header.arc_count:
            raise DeriveError("derived graph changed the topology size")
        if header.directed != base_header.directed:
            raise DeriveError("derived graph changed directedness")
        if header.coordinate_system != base_header.coordinate_system:
            raise DeriveError("derived graph changed the coordinate system")

        meta = _build_metadata(
            paths=paths, base_meta=base_meta, stats=stats, base_id=base_id,
            new_id=new_id, transform=parsed, seed=seed, graph_path=graph_path,
            base_graph=base_graph,
        )
        write_json(metadata_path, meta)
        created.append(metadata_path)
        manifests = _derive_manifests(paths, base_id, new_id, header)
        created.extend(manifests)
        _write_dataset_config(config_path, meta)
        created.append(config_path)
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        # A killed lpq_reweight may have left its partial file.
        Path(str(graph_path) + ".partial").unlink(missing_ok=True)
        raise

    return DerivedResult(
        dataset_id=new_id, base_id=base_id, transform=transform, seed=seed,
        graph_path=graph_path, metadata_path=metadata_path, config_path=config_path,
        manifest_paths=manifests, header=header, metadata=meta,
        reweight_summary=completed.stdout.strip(),
    )


# -----------------------------------------------------------------------------
# The fixed set
# -----------------------------------------------------------------------------


@dataclass
class DerivedEntry:
    dataset_id: str
    base: str
    transform: str
    seed: int
    pinned_checksum: int | None = None


def read_derived_manifest(path: Path) -> list[DerivedEntry]:
    with path.open("r", encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    entries = []
    for item in raw["datasets"]:
        entries.append(
            DerivedEntry(
                dataset_id=item["id"],
                base=item["base"],
                transform=str(item["transform"]),
                seed=int(item["seed"]),
                pinned_checksum=(
                    None
                    if item.get("graph_checksum_fnv1a64") is None
                    else int(item["graph_checksum_fnv1a64"])
                ),
            )
        )
    return entries


def write_derived_manifest(path: Path, entries: list[DerivedEntry], stats: dict[str, dict]) -> None:
    header = (
        "# The fixed set of DERIVED datasets: real topologies, SYNTHETIC weights.\n"
        "#\n"
        "# Every entry is reproducible from the repository:\n"
        "#   python -m lpq_tools.data.derive --from-manifest configs/derived_datasets.yaml\n"
        "# Seeds are explicit. graph_checksum_fnv1a64 pins the output; the integer-only\n"
        "# transforms (unit, uniform, scale, clip, bimodal) are exact on any platform,\n"
        "# lognormal and pareto use libm and are pinned to the generating host's libm.\n"
        "# Processed graphs (datasets/processed/) are git-ignored; this file, the\n"
        "# metadata and the query manifests are what is committed.\n"
        "# Derived graphs are never real road networks.\n"
    )
    body = {
        "datasets": [
            {
                "id": e.dataset_id,
                "base": e.base,
                "transform": e.transform,
                "seed": e.seed,
                "graph_checksum_fnv1a64": e.pinned_checksum,
                **stats.get(e.dataset_id, {}),
            }
            for e in entries
        ]
    }
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(header)
        yaml.safe_dump(body, handle, sort_keys=False, default_flow_style=False)


def derive_all(
    manifest_path: Path,
    *,
    paths: RepoPaths | None = None,
    progress: Callable[[str], None] | None = None,
    pin: bool = False,
) -> list[str]:
    """Builds every dataset in the manifest that is missing and verifies pins.

    Returns a list of problems (empty on success). An existing dataset is
    verified, never rebuilt. With ``pin`` the manifest is rewritten to record
    each dataset's checksum and summary weight statistics (n, max, mean); an
    entry that already has a different pin is reported, not overwritten.
    """
    paths = paths or RepoPaths.discover()
    entries = read_derived_manifest(manifest_path)
    problems: list[str] = []
    summaries: dict[str, dict] = {}
    for entry in entries:
        graph = paths.processed / f"{entry.dataset_id}.lpqgraph"
        if graph.exists():
            checksum = read_binary_graph_header(graph).checksum
        else:
            checksum = derive_dataset(
                entry.base, entry.transform, entry.seed, entry.dataset_id,
                paths=paths, progress=progress,
            ).checksum
        if entry.pinned_checksum is not None and entry.pinned_checksum != checksum:
            problems.append(
                f"{entry.dataset_id}: checksum {checksum} does not match the pinned "
                f"{entry.pinned_checksum}"
            )
            continue
        if pin:
            meta = read_json(paths.metadata / f"{entry.dataset_id}.json")
            entry.pinned_checksum = checksum
            summaries[entry.dataset_id] = {
                "node_count": meta["node_count"],
                "min_weight": meta["min_weight"],
                "max_weight": meta["max_weight"],
                "mean_weight": meta["mean_weight"],
            }
    if pin and not problems:
        write_derived_manifest(manifest_path, entries, summaries)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m lpq_tools.data.derive",
        description="Derive a same-topology, different-weights (synthetic) dataset.",
    )
    parser.add_argument("--base", help="real dataset id, e.g. dimacs_ny")
    parser.add_argument("--transform", help="e.g. uniform:1:64, lognormal:300:1.2, unit")
    parser.add_argument("--seed", type=int, help="explicit weight seed")
    parser.add_argument("--id", dest="new_id", help="new dataset id, must start with derived_")
    parser.add_argument("--from-manifest", type=Path,
                        help="derive every missing dataset listed in this YAML and verify pins")
    parser.add_argument("--pin", action="store_true",
                        help="with --from-manifest: record checksums and summary stats in it")
    args = parser.parse_args(argv)

    def say(message: str) -> None:
        print(message, flush=True)

    try:
        if args.from_manifest is not None:
            problems = derive_all(args.from_manifest, progress=say, pin=args.pin)
            for problem in problems:
                print(f"error: {problem}", file=sys.stderr)
            return 1 if problems else 0
        if None in (args.base, args.transform, args.seed, args.new_id):
            parser.error("--base, --transform, --seed and --id are required "
                         "(or use --from-manifest)")
        result = derive_dataset(args.base, args.transform, args.seed, args.new_id, progress=say)
    except (DeriveError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(result.reweight_summary)
    print(json.dumps({
        "dataset_id": result.dataset_id,
        "graph": str(result.graph_path),
        "metadata": str(result.metadata_path),
        "manifests": [str(p) for p in result.manifest_paths],
        "checksum": result.checksum,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
