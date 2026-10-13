"""The dataset pipeline (ESD.md section 26), end to end.

    download -> validate -> normalize -> convert to CSR -> compute metadata
    -> generate query manifest -> store checksum

Each step is a function in this module; :func:`prepare_dataset` runs them in
order for one dataset config. The normalize, convert and metadata steps are
delegated to the ``lpq_convert`` executable -- see the header of
``cpp/apps/lpq_convert.cpp`` for why the split falls there.

Every step is idempotent. Re-running a prepared dataset re-verifies what is on
disk and does no work, which is what makes the pipeline usable as a
precondition check at the start of an experiment rather than as a ceremony
somebody has to remember to perform.
"""

from __future__ import annotations

import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from lpq_tools.data.acquire import AcquiredFile, acquire_dataset, decompress
from lpq_tools.data.binary_header import BinaryGraphHeader, read_binary_graph_header
from lpq_tools.data.queries import build_manifest, write_manifest
from lpq_tools.data.sources import DatasetConfig
from lpq_tools.data.validate import ValidationReport, validate_raw_file
from lpq_tools.utilities.jsonio import read_json, write_json
from lpq_tools.utilities.paths import RepoPaths


class PipelineError(RuntimeError):
    """Raised when a pipeline step fails in a way the caller must handle."""


@dataclass
class PreparationResult:
    """What preparing one dataset produced."""

    dataset_id: str
    graph_path: Path
    metadata_path: Path
    manifest_path: Path
    header: BinaryGraphHeader

    acquired: list[AcquiredFile] = field(default_factory=list)
    validations: list[ValidationReport] = field(default_factory=list)

    converted: bool = False
    seconds: float = 0.0

    @property
    def trusted_files(self) -> list[str]:
        """Files accepted without a published checksum to verify against."""
        return [a.remote.filename for a in self.acquired if a.trusted_on_first_use]

    def summary(self) -> str:
        checksum = f"0x{self.header.checksum:016x}"
        action = "converted" if self.converted else "already current"
        line = (
            f"{self.dataset_id}: {self.header.node_count:,} nodes, "
            f"{self.header.arc_count:,} arcs, {checksum} ({action}, {self.seconds:.1f}s)"
        )
        if self.trusted_files:
            line += (
                "\n  note: accepted on trust, no published checksum to verify against: "
                + ", ".join(self.trusted_files)
            )
        return line


def _validate_inputs(
    config: DatasetConfig, graph_source: Path, coordinate_source: Path | None
) -> list[ValidationReport]:
    reports = [validate_raw_file(graph_source, config.converter_format)]
    if not reports[0].ok:
        raise PipelineError(
            f"{config.dataset_id}: {graph_source.name} failed validation:\n  "
            + "\n  ".join(reports[0].problems)
        )

    # The declaration is checked against the file rather than trusted. A SNAP
    # edge list gives no indication of which convention it uses, and reading a
    # doubly-listed file as singly-listed produces a graph with exactly twice
    # the edges -- structurally valid, silent, and wrong in every per-edge
    # measurement taken on it.
    if config.source_format == "snap":
        detected = reports[0].edges_listed
        if detected is not None and detected != config.edges_listed:
            raise PipelineError(
                f"{config.dataset_id}: the config declares edges_listed: "
                f"{config.edges_listed}, but {graph_source.name} appears to list each "
                f"edge '{detected}' ({reports[0].repeated_pairs} of "
                f"{reports[0].pair_sample} sampled records repeat an endpoint pair). "
                "Converting with the declared value would change the edge count by a "
                "factor of two."
            )

    if coordinate_source is not None:
        expected = reports[0].declared_nodes
        coord_report = validate_raw_file(
            coordinate_source, "dimacs_coordinates", expected_nodes=expected
        )
        reports.append(coord_report)
        if not coord_report.ok:
            raise PipelineError(
                f"{config.dataset_id}: {coordinate_source.name} failed validation:\n  "
                + "\n  ".join(coord_report.problems)
            )
    return reports


def _relative_to(root: Path, path: Path) -> str:
    """Expresses *path* relative to *root*, with forward slashes.

    Every path the converter is given, and therefore every path it records in
    the metadata, goes through here. Absolute paths would make two artifacts
    machine-specific that must not be: the metadata record, which is committed
    and describes the provenance of a graph, and the recorded command line,
    which is compared against a later run to decide whether reconversion is
    needed -- and which would otherwise force a reconversion for every checkout
    in a different directory.

    A path outside the repository (an LPQ_CONVERT override, say) is returned
    absolute, since there is nothing to make it relative to.
    """
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def _converter_command(
    config: DatasetConfig,
    converter: Path,
    graph_source: Path,
    coordinate_source: Path | None,
    graph_path: Path,
    metadata_path: Path,
    root: Path | None = None,
) -> list[str]:
    # Relative to the repository root, and the subprocess is run from there --
    # see _relative_to. Passing `root` is optional only so that callers which
    # merely want to inspect the flags do not have to supply one.
    rel = (lambda p: _relative_to(root, p)) if root is not None else str

    command = [
        rel(converter),
        "--input",
        rel(graph_source),
        "--format",
        config.converter_format,
        "--output",
        rel(graph_path),
        "--metadata",
        rel(metadata_path),
        "--dataset-id",
        config.dataset_id,
        "--directedness",
        config.directedness,
        "--self-loops",
        config.self_loops,
        "--parallel-edges",
        config.parallel_edges,
        "--default-weight",
        str(config.default_weight),
        "--edges-listed",
        config.edges_listed,
    ]
    if coordinate_source is not None:
        command += ["--coordinates", rel(coordinate_source)]
    if config.weights is not None:
        command += config.weights.to_arguments()
    return command


def _needs_conversion(
    graph_path: Path, metadata_path: Path, command: list[str], record_path: Path
) -> bool:
    """Decides whether the converter has to run again.

    The recorded command line is part of the decision, not just the presence of
    the output: changing a normalization policy or a weight seed in the config
    produces a different graph from the same inputs, and a pipeline that only
    checked for an existing file would keep serving the old one.
    """
    if not graph_path.exists() or not metadata_path.exists() or not record_path.exists():
        return True
    try:
        recorded = read_json(record_path)
    except (OSError, ValueError):
        return True
    # argv[0] is excluded: it names the executable, which differs between
    # platforms (lpq_convert vs lpq_convert.exe) and between build directories
    # without the resulting graph differing at all. Comparing it would force a
    # full reconversion of every dataset whenever the build moved.
    return list(recorded.get("command", []))[1:] != command[1:]


def prepare_dataset(
    config: DatasetConfig,
    paths: RepoPaths,
    *,
    force: bool = False,
    skip_download: bool = False,
    progress: Callable[[str], None] | None = None,
) -> PreparationResult:
    """Runs the whole pipeline for one dataset."""
    started = time.monotonic()
    paths.ensure_directories()

    def say(message: str) -> None:
        if progress is not None:
            progress(message)

    # -- download -------------------------------------------------------------
    say(f"{config.dataset_id}: acquiring {len(config.files)} file(s)")
    if skip_download:
        acquired = []
        for remote in config.files:
            candidate = paths.raw / remote.filename
            if not candidate.exists():
                raise PipelineError(
                    f"{config.dataset_id}: --skip-download was given but "
                    f"{candidate} is not present"
                )
    else:
        acquired = acquire_dataset(
            config, paths.raw, metadata_dir=paths.metadata, force=force
        )

    # -- normalize (part one): expand compressed archives ---------------------
    # Raw files stay exactly as published; the expansion lands in staging.
    graph_raw = paths.raw / config.graph_file.filename
    coordinate_source: Path | None = None
    osm_report: dict[str, Any] | None = None
    osm_ids: Path | None = None
    if config.source_format == "osm_pbf":
        # OSM: extract the road graph (documented policy in osm.py) into a
        # DIMACS pair in staging. The PBF itself is never modified.
        from lpq_tools.data.osm import extract_road_graph

        say(f"{config.dataset_id}: extracting the road graph from {graph_raw.name}")
        graph_source, coordinate_source, osm_ids, report = extract_road_graph(
            graph_raw, paths.staging / config.dataset_id
        )
        osm_report = report.as_dict()
    else:
        graph_source = decompress(graph_raw, paths.staging, force=force)
        if config.coordinate_file is not None:
            coordinate_source = decompress(
                paths.raw / config.coordinate_file.filename, paths.staging, force=force
            )

    # -- validate -------------------------------------------------------------
    say(f"{config.dataset_id}: validating")
    validations = _validate_inputs(config, graph_source, coordinate_source)

    # -- normalize (part two), convert, metadata, checksum --------------------
    graph_path = paths.processed / f"{config.dataset_id}.lpqgraph"
    metadata_path = paths.metadata / f"{config.dataset_id}.json"
    record_path = paths.metadata / f"{config.dataset_id}.conversion.json"

    converter = paths.converter()

    def rel_root(path: Path) -> str:
        return _relative_to(paths.root, path)

    command = _converter_command(
        config,
        converter,
        graph_source,
        coordinate_source,
        graph_path,
        metadata_path,
        root=paths.root,
    )

    converted = False
    if force or _needs_conversion(graph_path, metadata_path, command, record_path):
        say(f"{config.dataset_id}: converting")
        # argv[0] is recorded relative (portable) but executed absolute:
        # Windows resolves a relative executable name against the PARENT's
        # working directory, not the child's `cwd`, so a relative argv[0] here
        # fails with "cannot find the file specified".
        completed = subprocess.run(
            [str(converter), *command[1:]],
            capture_output=True,
            text=True,
            check=False,
            cwd=str(paths.root),
        )
        if completed.returncode != 0:
            raise PipelineError(
                f"{config.dataset_id}: lpq_convert failed (exit {completed.returncode})\n"
                f"{completed.stderr.strip()}"
            )
        converted = True
        write_json(
            record_path,
            {
                "dataset_id": config.dataset_id,
                "command": command,
                "converter": rel_root(converter),
                "stdout": completed.stdout.strip(),
            },
        )

    header = read_binary_graph_header(graph_path)

    # The metadata record and the file itself must agree. They are written by
    # the same process, so a disagreement means one of them was replaced
    # afterwards -- exactly the case where trusting the metadata would attribute
    # one graph's results to another.
    metadata = read_json(metadata_path)
    recorded_checksum = int(metadata.get("graph_checksum_fnv1a64", -1))
    if recorded_checksum != header.checksum:
        raise PipelineError(
            f"{config.dataset_id}: the metadata record claims checksum "
            f"0x{recorded_checksum:016x} but {graph_path.name} holds "
            f"0x{header.checksum:016x}. Re-run with force to rebuild both."
        )
    if int(metadata.get("node_count", -1)) != header.node_count:
        raise PipelineError(
            f"{config.dataset_id}: metadata node count disagrees with the graph file"
        )

    # -- OSM provenance (spec section 10.2) ------------------------------------
    if osm_report is not None and osm_ids is not None:
        from lpq_tools.utilities.hashing import sha256_file

        ids_target = paths.processed / f"{config.dataset_id}.osm_node_ids.u64"
        ids_target.write_bytes(osm_ids.read_bytes())
        write_json(
            paths.metadata / f"{config.dataset_id}.osm.json",
            {
                "dataset_id": config.dataset_id,
                "source_url": config.graph_file.url,
                "source_file": config.graph_file.filename,
                "source_sha256": sha256_file(graph_raw),
                "region": config.provenance.get("region"),
                "snapshot_date": config.provenance.get("snapshot_date"),
                "licence": config.provenance.get("licence"),
                "graph_conversion": {
                    "extractor": "lpq_tools.data.osm",
                    "converter": "lpq_convert --format dimacs",
                    **osm_report,
                },
                "osm_node_ids_path": rel_root(ids_target),
                "graph_checksum_fnv1a64": header.checksum,
            },
        )

    # -- generate query manifest ---------------------------------------------
    manifest_path = paths.manifests / f"{config.dataset_id}_queries.yaml"
    say(f"{config.dataset_id}: generating {config.query_count} queries")
    manifest = build_manifest(
        config.dataset_id,
        header.node_count,
        count=config.query_count,
        seed=config.query_seed,
        graph_checksum=header.checksum,
    )
    write_manifest(manifest_path, manifest)

    return PreparationResult(
        dataset_id=config.dataset_id,
        graph_path=graph_path,
        metadata_path=metadata_path,
        manifest_path=manifest_path,
        header=header,
        acquired=list(acquired),
        validations=validations,
        converted=converted,
        seconds=time.monotonic() - started,
    )


def prepare_all(
    configs: list[DatasetConfig],
    paths: RepoPaths,
    *,
    force: bool = False,
    skip_download: bool = False,
    progress: Callable[[str], None] | None = None,
) -> tuple[list[PreparationResult], list[tuple[str, Exception]]]:
    """Prepares every dataset, continuing past failures.

    One unavailable mirror should not prevent the other five datasets from being
    prepared, so failures are collected and returned alongside the successes
    rather than aborting the batch. The caller decides what a partial result
    means.
    """
    results: list[PreparationResult] = []
    failures: list[tuple[str, Exception]] = []
    for config in configs:
        try:
            results.append(
                prepare_dataset(
                    config,
                    paths,
                    force=force,
                    skip_download=skip_download,
                    progress=progress,
                )
            )
        except Exception as exc:  # noqa: BLE001 -- reported, not swallowed
            failures.append((config.dataset_id, exc))
    return results, failures


def summarise(results: list[PreparationResult], root: Path | None = None) -> dict[str, Any]:
    """A machine-readable index of everything that was prepared.

    Paths are recorded relative to *root* when one is given, for the same
    reason the metadata records are (see _relative_to): this file is committed.
    """
    def path_of(path: Path) -> str:
        return _relative_to(root, path) if root is not None else str(path)

    return {
        "datasets": [
            {
                "dataset_id": r.dataset_id,
                "graph_path": path_of(r.graph_path),
                "metadata_path": path_of(r.metadata_path),
                "manifest_path": path_of(r.manifest_path),
                "node_count": r.header.node_count,
                "arc_count": r.header.arc_count,
                "edge_count": r.header.edge_count,
                "directed": r.header.directed,
                "weight_type": r.header.weight_type,
                "has_coordinates": r.header.has_coordinates,
                "graph_checksum_fnv1a64": r.header.checksum,
                "files_accepted_on_trust": r.trusted_files,
            }
            for r in sorted(results, key=lambda r: r.dataset_id)
        ]
    }


def _index_entries(index_path: Path, document: Any) -> dict[str, dict[str, Any]]:
    """The entries of a parsed index keyed by id, or PipelineError if it is malformed."""
    listed = document.get("datasets") if isinstance(document, dict) else None
    if not isinstance(listed, list):
        raise PipelineError(
            f"{index_path} is not an index: expected an object with a 'datasets' list. "
            "It was not overwritten."
        )
    entries: dict[str, dict[str, Any]] = {}
    for item in listed:
        dataset_id = item.get("dataset_id") if isinstance(item, dict) else None
        if not isinstance(dataset_id, str):
            raise PipelineError(
                f"{index_path} has an entry without a string 'dataset_id'. It was not overwritten."
            )
        if dataset_id in entries:
            raise PipelineError(
                f"{index_path} lists '{dataset_id}' twice. It was not overwritten."
            )
        entries[dataset_id] = item
    return entries


def update_index(index_path: Path, summary: dict[str, Any]) -> dict[str, Any]:
    """Merges *summary* (the output of :func:`summarise`) into the index at *index_path*.

    ``datasets/metadata/index.json`` is committed and lists every prepared dataset. Writing
    it from only the datasets one run prepared deleted all the others, so preparing a single
    dataset appeared in ``git diff`` as the removal of the rest. This replaces the entries for
    the datasets in *summary*, adds any that are new, and leaves every other entry as it was,
    keeping the list sorted by id. Any other top-level key is kept too.

    The index is parsed and written back with :func:`write_json`, so entries left alone stay
    byte for byte identical when the file was itself written that way, which is how the
    pipeline writes it.

    An index that cannot be understood (invalid JSON, the wrong shape, an entry without an
    id, a repeated id) raises :class:`PipelineError` and is **not** overwritten: replacing a
    record the pipeline cannot read would destroy information to make a run look tidy.
    When *summary* holds no datasets the file is not touched.

    Returns the merged index.
    """
    existing: dict[str, Any] = {}
    entries: dict[str, dict[str, Any]] = {}
    if index_path.exists():
        try:
            document = read_json(index_path)
        except ValueError as exc:  # JSONDecodeError and UnicodeDecodeError both derive from it
            raise PipelineError(
                f"{index_path} is not valid JSON ({exc}). It was not overwritten."
            ) from exc
        entries = _index_entries(index_path, document)
        existing = {k: v for k, v in document.items() if k != "datasets"}

    prepared = _index_entries(index_path, {"datasets": summary.get("datasets", [])})
    combined = {**entries, **prepared}  # a prepared entry replaces the one with the same id
    merged = {**existing, "datasets": [combined[i] for i in sorted(combined)]}
    if prepared:
        write_json(index_path, merged)
    return merged
