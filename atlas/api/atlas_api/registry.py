"""Dataset, model and capability registry (spec sections 16, 17, 19, 49).

Built entirely from artifacts on disk -- converted graphs, their metadata
records, dataset configs, query manifests and model reports -- so every value
the UI shows can be traced to a file the research pipeline wrote. Nothing here
is a hand-maintained list of datasets.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from typing import Any

import yaml
from atlas_api.config import Settings
from lpq_tools.data.binary_header import BinaryGraphError, read_binary_graph_header
from lpq_tools.training.leakage import check_split
from lpq_tools.utilities.hashing import sha256_file
from lpq_tools.utilities.jsonio import read_json

ALGORITHMS = [
    {
        "id": "dijkstra",
        "name": "Dijkstra",
        "description": (
            "Label-setting shortest path from the source; stops when the target is settled."
        ),
        "requires_coordinates": False,
    },
    {
        "id": "astar",
        "name": "A*",
        "description": (
            "Dijkstra ordered by g + h. On geographic graphs h is the Haversine distance scaled by "
            "the largest factor that keeps it admissible and consistent for this graph's weights."
        ),
        "requires_coordinates": False,
        "heuristics": ["auto", "zero", "haversine", "euclidean"],
    },
]

QUEUES: list[dict[str, Any]] = [
    {"id": "binary_heap", "name": "Binary heap", "family": "classical", "decrease_key": False},
    {"id": "four_ary_heap", "name": "4-ary heap", "family": "classical", "decrease_key": False},
    {"id": "pairing_heap", "name": "Pairing heap", "family": "classical", "decrease_key": True},
    {"id": "fibonacci_heap", "name": "Fibonacci heap", "family": "classical", "decrease_key": True},
    {"id": "radix_heap", "name": "Radix heap", "family": "classical", "decrease_key": False},
    {
        "id": "lpq",
        "name": "LPQ (control scorer)",
        "family": "learned",
        "scorer": "constant_control_no_model",
        "description": (
            "The learned-queue machinery with every node scored identically: measures the "
            "bucket substrate, candidate window, verification and correction without a model."
        ),
    },
    {
        "id": "lpq_model",
        "name": "LPQ + static model",
        "family": "learned",
        "scorer": "static_precomputed_model_scores",
        "requires_model": True,
        "description": "Scores precomputed once per node from static graph features by a trained "
        "model; the hot path reads one float per push. No online learning.",
    },
    {
        "id": "lpq_adaptive",
        "name": "LPQ + adaptive controller",
        "family": "learned",
        "scorer": "constant_control_no_model",
        "description": "The control-scorer LPQ with the adaptive controller attached; optional "
        "online learning of the policy model from verified outcomes.",
    },
    {
        "id": "lpq_online",
        "name": "LPQ + online learner (between queries)",
        "family": "learned",
        "requires_model": True,
        "scorer": "online_logistic_static_features_on_demand",
        "description": (
            "Scores pushes with a model you choose explicitly, records what the verifier "
            "established, and applies updates after the search, so the search itself runs on the "
            "starting model. A single run applies at most one round of updates; nothing here says "
            "an update helps."
        ),
    },
    {
        "id": "lpq_optimized",
        "name": "Optimized (ML-assisted)",
        "family": "learned",
        "exact": True,
        "requires_model": False,
        "scorer": "engine_selected",
        "description": (
            "A priority queue that uses a learned hint to keep the next nodes to visit in "
            "small buckets, and checks every hint against an exact fallback, so the answer is "
            "always the exact shortest distance. The engine picks its own configuration: "
            "there is no model file or setting to choose."
        ),
    },
]

_FAMILIES = {"osm": "OSM", "dimacs": "DIMACS", "snap": "SNAP", "synth": "synthetic"}


def family_of(dataset_id: str) -> str:
    return _FAMILIES.get(dataset_id.split("_", 1)[0], "other")


@dataclass
class Dataset:
    id: str
    family: str
    status: str
    info: dict[str, Any] = field(default_factory=dict)
    graph_path: Path | None = None

    @property
    def ready(self) -> bool:
        return self.status == "READY"


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data if isinstance(data, dict) else {}


def _manifest_summary(path: Path) -> dict[str, Any]:
    # Only the header fields: the query list is read on demand.
    head: dict[str, Any] = {}
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("queries:"):
                break
            if ":" in line and not line.startswith("#"):
                k, v = line.split(":", 1)
                head[k.strip()] = v.strip()
    return {
        "file": path.name,
        "role": "training" if "training" in path.stem else "evaluation",
        "seed": int(head.get("seed", 0) or 0),
        "query_count": int(head.get("query_count", 0) or 0),
    }


class Registry:
    def __init__(self, settings: Settings) -> None:
        self.s = settings
        self.root = settings.root

    # -- datasets ------------------------------------------------------------

    def datasets(self) -> list[Dataset]:
        ids: set[str] = set()
        meta_dir = self.root / "datasets" / "metadata"
        for p in meta_dir.glob("*.json"):
            stem = p.stem
            if "." in stem or stem == "index":
                continue
            ids.add(stem)
        for p in (self.root / "configs" / "datasets").glob("*.yaml"):
            ids.add(p.stem)
        return [self.dataset(i) for i in sorted(ids)]

    def dataset(self, dataset_id: str) -> Dataset:
        meta_path = self.root / "datasets" / "metadata" / f"{dataset_id}.json"
        cfg_path = self.root / "configs" / "datasets" / f"{dataset_id}.yaml"
        graph = self.root / "datasets" / "processed" / f"{dataset_id}.lpqgraph"
        if not meta_path.exists() and not cfg_path.exists():
            raise KeyError(dataset_id)
        info: dict[str, Any] = {"id": dataset_id, "family": family_of(dataset_id)}
        cfg = _load_yaml(cfg_path) if cfg_path.exists() else {}
        if cfg:
            info["description"] = " ".join(str(cfg.get("description", "")).split())
            info["source_format"] = cfg.get("source_format")
            info["provenance"] = cfg.get("provenance", {})
            info["source_urls"] = [f.get("url") for f in cfg.get("files", [])]
            info["weights_synthesized"] = cfg.get("weights") is not None
        status = "NOT_DOWNLOADED"
        if graph.exists() and meta_path.exists():
            meta = read_json(meta_path)
            try:
                header = read_binary_graph_header(graph)
                if int(meta.get("graph_checksum_fnv1a64", -1)) != header.checksum:
                    status = "FAILED"
                    info["status_detail"] = "graph file does not match its metadata checksum"
                else:
                    status = "READY"
                info["coordinate_system"] = header.coordinate_system
                info["format_version"] = header.version
            except BinaryGraphError as e:
                status = "FAILED"
                info["status_detail"] = str(e).split(": ", 1)[-1]
            info.update(
                nodes=meta.get("node_count"),
                edges=meta.get("edge_count"),
                arcs=meta.get("arc_count"),
                directed=meta.get("directed"),
                weighted=not (meta.get("min_weight") == meta.get("max_weight") == 1),
                min_weight=meta.get("min_weight"),
                max_weight=meta.get("max_weight"),
                mean_weight=meta.get("mean_weight"),
                has_coordinates=meta.get("has_coordinates"),
                checksum_fnv1a64=str(meta.get("graph_checksum_fnv1a64")),
                normalization=meta.get("normalization"),
            )
            if meta.get("weights", {}).get("synthesized"):
                info["weights_synthesized"] = True
        elif (self.root / "datasets" / "raw").exists() and cfg:
            raw_present = all(
                (self.root / "datasets" / "raw" / Path(str(f.get("url", ""))).name).exists()
                for f in cfg.get("files", [])
            )
            if raw_present:
                status = "NOT_CONVERTED"
        osm_path = self.root / "datasets" / "metadata" / f"{dataset_id}.osm.json"
        if osm_path.exists():
            osm = read_json(osm_path)
            conv = osm.get("graph_conversion", {})
            info["osm"] = {
                "region": osm.get("region"),
                "snapshot_date": osm.get("snapshot_date"),
                "source_url": osm.get("source_url"),
                "source_sha256": osm.get("source_sha256"),
                "licence": osm.get("licence"),
                "road_policy_version": conv.get("road_policy_version"),
                "weight_unit": conv.get("weight_unit"),
                "turn_restrictions_modelled": conv.get("turn_restrictions_modelled"),
                "ways_kept": conv.get("ways_kept"),
                "ways_excluded": conv.get("ways_excluded"),
                "nodes_dropped_outside_largest_scc": conv.get("nodes_dropped_outside_largest_scc"),
                "bbox": conv.get("bbox"),
                "extractor": conv.get("extractor"),
            }
        manifests = sorted((self.root / "experiments" / "manifests").glob(f"{dataset_id}_*.yaml"))
        info["query_manifests"] = [_manifest_summary(m) for m in manifests]
        info["compatible_algorithms"] = ["dijkstra", "astar"]
        info["astar_heuristics"] = (
            ["auto", "haversine", "zero"]
            if info.get("coordinate_system") == "geographic"
            else ["auto", "euclidean", "zero"]
            if info.get("coordinate_system") == "euclidean"
            else ["zero"]
        )
        nodes = int(info.get("nodes") or 0)
        info["scale_mode"] = (
            "research"
            if not info.get("has_coordinates")
            else "teaching"
            if nodes <= 20_000
            else "route"
            if nodes <= 400_000
            else "research"
        )
        info["models"] = [m["id"] for m in self.models() if m.get("training_dataset") == dataset_id]
        info["status"] = status
        return Dataset(
            dataset_id, info["family"], status, info, graph if status == "READY" else None
        )

    def manifest_path(self, dataset_id: str, file: str) -> Path:
        # The file name is matched against the directory listing, never joined
        # from the request: path traversal has nothing to traverse.
        for m in (self.root / "experiments" / "manifests").glob(f"{dataset_id}_*.yaml"):
            if m.name == file:
                return m
        raise KeyError(file)

    # -- models --------------------------------------------------------------

    def models(self) -> list[dict[str, Any]]:
        return [
            self._model(p) for p in sorted((self.root / "models" / "exported").glob("*.lpqmodel"))
        ]

    def model(self, model_id: str) -> dict[str, Any]:
        for m in self.models():
            if m["id"] == model_id:
                return m
        raise KeyError(model_id)

    def model_path(self, model_id: str) -> Path:
        p = self.root / "models" / "exported" / f"{model_id}.lpqmodel"
        if not p.exists() or p.parent.resolve() != (self.root / "models" / "exported").resolve():
            raise KeyError(model_id)
        return p

    def _model(self, path: Path) -> dict[str, Any]:
        return _model_info(str(path), path.stat().st_mtime, str(self.s.tool("lpq_model_info")))


def _training_manifest(
    report: dict[str, Any], dataset: str | None, manifests: Path
) -> tuple[Path | None, dict[str, Any]]:
    """Which query manifest trained this model, and how far that can be trusted.

    A report written by the current trainer records the manifest's name and SHA-256, and that
    record is used: the file is looked up by the recorded name and accepted only if its hash still
    matches. If it is missing or has changed, nothing is substituted and the split is UNKNOWN. A
    report from before the record existed falls back to the old dataset-named file and says so,
    because that file is a guess about which manifest was used.
    """
    recorded = report.get("training_manifest")
    if isinstance(recorded, dict) and recorded.get("name"):
        info: dict[str, Any] = {
            "recorded": True,
            "source": "recorded",
            "name": recorded["name"],
            "sha256": recorded.get("sha256"),
            "selection": recorded.get("selection"),
        }
        candidate = manifests / Path(str(recorded["name"])).name
        if not candidate.is_file():
            info["status"] = "manifest_missing"
            return None, info
        if sha256_file(candidate) != recorded.get("sha256"):
            info["status"] = "hash_mismatch"
            return None, info
        info["status"] = "verified"
        return candidate, info
    info = {"recorded": False, "source": "none", "status": "not_recorded"}
    legacy = manifests / f"{dataset}_training_queries.yaml" if dataset else None
    if legacy is not None and legacy.is_file():
        info["source"] = "inferred_from_dataset_filename"
        info["name"] = legacy.name
        info["note"] = (
            "This report does not record its training manifest; the file was inferred from the "
            "dataset name and may not be the one used."
        )
        return legacy, info
    return None, info


@lru_cache(maxsize=32)
def _model_info(path_s: str, _mtime: float, info_tool: str) -> dict[str, Any]:
    path = Path(path_s)
    model_id = path.stem
    report_path = path.with_name(f"{model_id}.report.json")
    report = read_json(report_path) if report_path.exists() else {}
    out: dict[str, Any] = {
        "id": model_id,
        "file": path.name,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        "implementation_available": True,
        "trained_artifact": True,
        "training_dataset": report.get("dataset_id"),
        "report": report,
    }
    # The C++ loader is the authority on what the file contains.
    try:
        # argv is [registry tool path, fixed flag, registry model path]: no request data.
        proc = subprocess.run(  # noqa: S603
            [info_tool, "--model", str(path)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        fields: dict[str, str] = {}
        weights: dict[str, float] = {}
        for line in proc.stdout.splitlines():
            k, _, v = line.partition(":")
            k, v = k.strip(), v.strip()
            if k.startswith("weight."):
                weights[k[7:]] = float(v)
            elif k:
                fields[k] = v
        out["kind"] = fields.get("kind")
        out["feature_layout_version"] = fields.get("feature_layout_version")
        out["feature_count"] = int(fields["feature_count"]) if "feature_count" in fields else None
        out["native_weights"] = weights
        out["used_features"] = [k for k, v in weights.items() if v != 0.0]
        out["loader_ok"] = proc.returncode == 0
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        out["loader_ok"] = False
        out["loader_error"] = type(e).__name__
    out["mode"] = "static_precomputed_scores"
    out["mode_explanation"] = (
        "Scores are computed once per node from static graph features before the search "
        "(StaticNodeScorer); the search reads one precomputed float per push. The model is "
        "not updated during or across searches."
    )
    dataset = report.get("dataset_id")
    manifests = path.parents[2] / "experiments" / "manifests"
    training, provenance = _training_manifest(report, dataset, manifests)
    out["training_manifest_provenance"] = provenance
    splits = []
    if training is not None:
        out["training_manifest"] = training.name
        for ev in sorted(manifests.glob(f"{dataset}_queries*.yaml")):
            splits.append(check_split(training, ev).as_dict())
    elif provenance.get("name"):
        out["training_manifest"] = provenance["name"]
    out["splits"] = splits
    out["split_status"] = (
        "UNKNOWN"
        if not splits
        else "DISJOINT"
        if all(s["status"] == "DISJOINT" for s in splits)
        else "OVERLAPPING"
    )
    return out


@lru_cache(maxsize=8)
def _engine_help_queues(tool: str, _mtime: float) -> frozenset[str] | None:
    """Queue ids named in the ``--pq`` entry of ``lpq_run --help``, or None if unreadable.

    lpq_run has no machine-readable listing, and its help text is the engine's own
    statement of what it accepts. Keyed by binary mtime, so a rebuild is noticed.
    """
    try:
        proc = subprocess.run(  # noqa: S603 -- registry tool path, one fixed flag
            [tool, "--help"], capture_output=True, text=True, timeout=15, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return None
    lines = (proc.stdout + proc.stderr).splitlines()
    block: list[str] = []
    for i, line in enumerate(lines):
        if line.strip().startswith("--pq"):
            block.append(line)
            for nxt in lines[i + 1 :]:
                if nxt.strip().startswith("--") or not nxt.strip():
                    break
                block.append(nxt)
            break
    if not block:
        return None
    text = " ".join(block)
    return frozenset(q["id"] for q in QUEUES if re.search(rf"(?<![\w-]){q['id']}(?![\w-])", text))


def engine_queue_ids(settings: Settings) -> frozenset[str] | None:
    """Queues the installed lpq_run says it accepts; None when that cannot be determined."""
    try:
        tool = settings.tool("lpq_run")
        return _engine_help_queues(str(tool), tool.stat().st_mtime)
    except (FileNotFoundError, OSError):
        return None


def queues_with_availability(settings: Settings) -> list[dict[str, Any]]:
    """The queue registry plus ``engine_available``: True, False, or None when unknown."""
    supported = engine_queue_ids(settings)
    return [
        {**q, "engine_available": None if supported is None else q["id"] in supported}
        for q in QUEUES
    ]


def capabilities(settings: Settings) -> dict[str, Any]:
    tools = {}
    for name in ("lpq_run", "lpq_benchmark", "lpq_export", "lpq_model_info"):
        try:
            settings.tool(name)
            tools[name] = True
        except FileNotFoundError:
            tools[name] = False
    supported = engine_queue_ids(settings)
    return {
        "engine_tools": tools,
        "engine_queues": None if supported is None else sorted(supported),
        "trace_levels": ["none", "summary", "visual", "full"],
        "max_trace_events": settings.max_trace_events,
        "max_concurrent_jobs": settings.max_concurrent_jobs,
        "run_timeout_s": settings.run_timeout_s,
        "correctness_policies": ["exact", "approximate"],
        "epsilon_range": [0.0, 1.0],
        "hardware_counters": {
            "available": False,
            "reason": "Unavailable on this execution environment (no CPU PMU exposed to the "
            "engine); never estimated.",
        },
        "memory_metric": {
            "available": False,
            "reason": "No per-query memory measurement is defined for lpq_run; not shown.",
        },
        "turn_restrictions": False,
        "weights": "OSM weights are great-circle decimetres: routes are shortest, not fastest.",
    }
