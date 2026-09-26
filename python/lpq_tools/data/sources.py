"""Dataset declarations (PRD.md section 17, ESD.md section 26).

A dataset is a YAML file in ``configs/datasets/``, not a Python constant. That
boundary is what makes the pipeline generic: adding the full DIMACS USA road
network is a new config file, and no code changes.

-------------------------------------------------------------------------------
On checksums that are not yet known
-------------------------------------------------------------------------------

A config may declare ``sha256`` for each file it downloads. Where it does, a
download whose digest disagrees is rejected outright.

Where it does not, the digest CANNOT be invented -- writing a plausible-looking
hash for a file nobody has fetched would make the verification worse than
useless, because it would fail for the honest reason (wrong file) and for the
dishonest one (wrong expectation) identically, and the second is invisible. So
an unpinned file is downloaded, its observed digest is recorded in a lock file
next to the dataset, and the run says clearly that the file was accepted on
trust. Pinning it afterwards is a one-line edit to the config, and from then on
the dataset is verified.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import yaml

#: Source formats ``lpq_convert`` understands.
_FORMATS = ("snap", "dimacs", "binary", "osm_pbf")

#: Weight families ``weight_distributions.hpp`` understands. Duplicated here so
#: a typo is caught while reading the config rather than after a download.
_WEIGHT_DISTRIBUTIONS = (
    "constant",
    "uniform",
    "lognormal",
    "exponential",
    "narrow_range",
    "heavy_tailed",
)


class DatasetConfigError(ValueError):
    """Raised when a dataset config is malformed or internally inconsistent."""


@dataclass(frozen=True)
class RemoteFile:
    """One file to fetch.

    Attributes:
        url: Where to fetch it from.
        filename: Name to store it under in ``datasets/raw``. Defaults to the
            last path component of the URL.
        sha256: Expected digest, or ``None`` when not yet pinned.
        role: ``"graph"`` for the edge/arc file, ``"coordinates"`` for a DIMACS
            ``.co`` file. Anything else is rejected, so a config cannot quietly
            download something the pipeline will then ignore.
    """

    url: str
    filename: str
    sha256: str | None = None
    role: str = "graph"

    @property
    def is_compressed(self) -> bool:
        return self.filename.endswith(".gz")

    @property
    def decompressed_name(self) -> str:
        return self.filename[:-3] if self.is_compressed else self.filename


@dataclass(frozen=True)
class WeightSynthesis:
    """A synthetic weight assignment, for datasets published unweighted.

    Mirrors ``lpq::WeightDistributionConfig``. Held as data here and passed to
    the converter as flags, so the sampler itself exists once, in C++.
    """

    distribution: str
    minimum: float
    maximum: float
    seed: int
    sigma: float | None = None
    alpha: float | None = None
    narrow_fraction: float | None = None

    def to_arguments(self) -> list[str]:
        args = [
            "--weights",
            self.distribution,
            "--weight-min",
            repr(float(self.minimum)),
            "--weight-max",
            repr(float(self.maximum)),
            "--weight-seed",
            str(int(self.seed)),
        ]
        if self.sigma is not None:
            args += ["--weight-sigma", repr(float(self.sigma))]
        if self.alpha is not None:
            args += ["--weight-alpha", repr(float(self.alpha))]
        if self.narrow_fraction is not None:
            args += ["--narrow-fraction", repr(float(self.narrow_fraction))]
        return args


@dataclass(frozen=True)
class DatasetConfig:
    """A complete dataset declaration."""

    dataset_id: str
    description: str
    source_format: str
    files: tuple[RemoteFile, ...]

    directedness: str = "undirected"
    self_loops: str = "drop"
    parallel_edges: str = "keep"
    default_weight: int = 1

    #: Whether the source file lists each undirected edge once or gives both
    #: directions. SNAP is not consistent about this and the file says nothing
    #: either way, so it is declared and then checked against the file during
    #: validation. Getting it wrong doubles the edge count silently.
    edges_listed: str = "once"

    weights: WeightSynthesis | None = None

    #: Seed for the query manifest. Fixed in the config so the same source and
    #: target pairs are regenerated on every machine (PRD.md section 18).
    query_seed: int = 42
    query_count: int = 1000

    #: Free-form provenance: citation, licence, collection date.
    provenance: dict[str, Any] = field(default_factory=dict)

    @property
    def graph_file(self) -> RemoteFile:
        for remote in self.files:
            if remote.role == "graph":
                return remote
        raise DatasetConfigError(f"{self.dataset_id}: no file with role 'graph'")

    @property
    def coordinate_file(self) -> RemoteFile | None:
        for remote in self.files:
            if remote.role == "coordinates":
                return remote
        return None

    @property
    def converter_format(self) -> str:
        """The format ``lpq_convert`` is given.

        An OSM snapshot is not converted directly: ``lpq_tools.data.osm``
        extracts its road graph into DIMACS first, so it reaches the one
        converter every other dataset uses.
        """
        return "dimacs" if self.source_format == "osm_pbf" else self.source_format

    @property
    def unpinned_files(self) -> tuple[RemoteFile, ...]:
        return tuple(f for f in self.files if f.sha256 is None)


def _require(mapping: dict[str, Any], key: str, dataset_id: str) -> Any:
    if key not in mapping:
        raise DatasetConfigError(f"{dataset_id}: missing required key '{key}'")
    return mapping[key]


def _parse_weights(raw: dict[str, Any] | None, dataset_id: str) -> WeightSynthesis | None:
    if raw is None:
        return None
    distribution = _require(raw, "distribution", dataset_id)
    if distribution not in _WEIGHT_DISTRIBUTIONS:
        raise DatasetConfigError(
            f"{dataset_id}: unknown weight distribution '{distribution}'; "
            f"expected one of {', '.join(_WEIGHT_DISTRIBUTIONS)}"
        )
    minimum = float(_require(raw, "minimum", dataset_id))
    maximum = float(_require(raw, "maximum", dataset_id))
    if minimum < 0.0:
        raise DatasetConfigError(f"{dataset_id}: weight minimum must be non-negative")
    if maximum < minimum:
        raise DatasetConfigError(f"{dataset_id}: weight maximum is below the minimum")

    return WeightSynthesis(
        distribution=distribution,
        minimum=minimum,
        maximum=maximum,
        # No default: an unseeded synthesis is not reproducible, and a default
        # would hide that the config forgot to say which graph it meant.
        seed=int(_require(raw, "seed", dataset_id)),
        sigma=None if raw.get("sigma") is None else float(raw["sigma"]),
        alpha=None if raw.get("alpha") is None else float(raw["alpha"]),
        narrow_fraction=(
            None if raw.get("narrow_fraction") is None else float(raw["narrow_fraction"])
        ),
    )


def parse_dataset_config(raw: dict[str, Any]) -> DatasetConfig:
    """Builds a :class:`DatasetConfig` from a parsed YAML mapping."""
    if not isinstance(raw, dict):
        raise DatasetConfigError("dataset config must be a mapping")

    dataset_id = raw.get("dataset_id")
    if not dataset_id or not isinstance(dataset_id, str):
        raise DatasetConfigError("dataset config must declare a string 'dataset_id'")

    source_format = _require(raw, "source_format", dataset_id)
    if source_format not in _FORMATS:
        raise DatasetConfigError(
            f"{dataset_id}: unknown source_format '{source_format}'; "
            f"expected one of {', '.join(_FORMATS)}"
        )

    raw_files = _require(raw, "files", dataset_id)
    if not isinstance(raw_files, list) or not raw_files:
        raise DatasetConfigError(f"{dataset_id}: 'files' must be a non-empty list")

    files: list[RemoteFile] = []
    seen: set[str] = set()
    for entry in raw_files:
        if not isinstance(entry, dict):
            raise DatasetConfigError(f"{dataset_id}: every entry of 'files' must be a mapping")
        url = _require(entry, "url", dataset_id)
        filename = entry.get("filename") or url.rsplit("/", 1)[-1]
        if not filename:
            raise DatasetConfigError(f"{dataset_id}: cannot derive a filename from '{url}'")
        if filename in seen:
            # Two entries writing the same raw file would race, and the second
            # would appear to succeed against the first one's contents.
            raise DatasetConfigError(f"{dataset_id}: duplicate raw filename '{filename}'")
        seen.add(filename)

        role = entry.get("role", "graph")
        if role not in ("graph", "coordinates"):
            raise DatasetConfigError(
                f"{dataset_id}: unknown file role '{role}' (expected graph or coordinates)"
            )
        digest = entry.get("sha256")
        if digest is not None:
            digest = str(digest).strip().lower()
            if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise DatasetConfigError(
                    f"{dataset_id}: '{filename}' has a sha256 that is not 64 hex characters"
                )
        files.append(RemoteFile(url=url, filename=filename, sha256=digest, role=role))

    roles = [f.role for f in files]
    if roles.count("graph") != 1:
        raise DatasetConfigError(f"{dataset_id}: exactly one file must have role 'graph'")
    if roles.count("coordinates") > 1:
        raise DatasetConfigError(f"{dataset_id}: at most one file may have role 'coordinates'")

    directedness = raw.get("directedness", "undirected")
    if directedness not in ("directed", "undirected"):
        raise DatasetConfigError(f"{dataset_id}: unknown directedness '{directedness}'")
    self_loops = raw.get("self_loops", "drop")
    if self_loops not in ("keep", "drop"):
        raise DatasetConfigError(f"{dataset_id}: unknown self_loops policy '{self_loops}'")
    edges_listed = raw.get("edges_listed", "once")
    if edges_listed not in ("once", "both"):
        raise DatasetConfigError(
            f"{dataset_id}: edges_listed must be 'once' or 'both', got '{edges_listed}'"
        )
    parallel_edges = raw.get("parallel_edges", "keep")
    if parallel_edges not in ("keep", "keep_minimum"):
        raise DatasetConfigError(
            f"{dataset_id}: unknown parallel_edges policy '{parallel_edges}'"
        )

    query_count = int(raw.get("query_count", 1000))
    if query_count <= 0:
        raise DatasetConfigError(f"{dataset_id}: query_count must be positive")

    config = DatasetConfig(
        dataset_id=dataset_id,
        description=str(raw.get("description", "")),
        source_format=source_format,
        files=tuple(files),
        directedness=directedness,
        self_loops=self_loops,
        parallel_edges=parallel_edges,
        edges_listed=edges_listed,
        default_weight=int(raw.get("default_weight", 1)),
        weights=_parse_weights(raw.get("weights"), dataset_id),
        query_seed=int(raw.get("query_seed", 42)),
        query_count=query_count,
        provenance=dict(raw.get("provenance", {})),
    )

    if config.coordinate_file is not None and config.source_format != "dimacs":
        raise DatasetConfigError(
            f"{dataset_id}: a coordinates file is only meaningful for the DIMACS format"
        )
    return config


def load_dataset_config(path: Path) -> DatasetConfig:
    with path.open("r", encoding="utf-8") as handle:
        return parse_dataset_config(yaml.safe_load(handle))


def load_all_dataset_configs(directory: Path) -> list[DatasetConfig]:
    """Loads every ``*.yaml`` in *directory*, sorted by dataset id."""
    configs = [load_dataset_config(p) for p in sorted(directory.glob("*.yaml"))]
    _reject_duplicate_ids(configs)
    return configs


def _reject_duplicate_ids(configs: Iterable[DatasetConfig]) -> None:
    seen: dict[str, str] = {}
    for config in configs:
        if config.dataset_id in seen:
            raise DatasetConfigError(
                f"two configs declare dataset_id '{config.dataset_id}'; "
                "processed graphs and metadata are keyed by it, so one would "
                "silently overwrite the other"
            )
        seen[config.dataset_id] = config.dataset_id
