"""Node coordinates and topology for map rendering and snapping (spec 10.4-10.6).

The arrays come from ``lpq_export``, i.e. from the C++ reader that owns the
``.lpqgraph`` format, and are cached per graph checksum so a reconverted graph
can never be served stale coordinates. Map clicks are snapped to the nearest
node of the processed graph -- the graph the algorithm actually runs on, not
the tile layer underneath it.
"""

from __future__ import annotations

import json
import math
import shutil
import subprocess
import threading
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from atlas_api.config import Settings

_EARTH_R = 6371008.8


class GraphUnavailable(RuntimeError):
    pass


@dataclass
class GraphArrays:
    dataset_id: str
    checksum: str
    nodes: int
    directed: bool
    coordinate_system: str
    coords: np.ndarray | None  # (n, 2): (lon, lat) for geographic graphs
    offsets: np.ndarray
    targets: np.ndarray
    weights: np.ndarray
    osm_ids: np.ndarray | None
    _arc_sources: np.ndarray | None = None

    @property
    def arc_sources(self) -> np.ndarray:
        if self._arc_sources is None:
            self._arc_sources = np.repeat(
                np.arange(self.nodes, dtype=np.int64), np.diff(self.offsets).astype(np.int64)
            )
        return self._arc_sources


def haversine_m(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = (
        math.sin((p2 - p1) / 2) ** 2
        + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    )
    return 2 * _EARTH_R * math.asin(math.sqrt(min(1.0, a)))


class GraphCache:
    def __init__(self, settings: Settings) -> None:
        self.s = settings
        self._lock = threading.Lock()
        self._loaded: dict[str, GraphArrays] = {}

    def get(self, dataset_id: str, graph_path: Path, checksum: str) -> GraphArrays:
        with self._lock:
            cached = self._loaded.get(dataset_id)
            if cached is not None and cached.checksum == checksum:
                return cached
            arrays = self._load(dataset_id, graph_path, checksum)
            self._loaded[dataset_id] = arrays
            return arrays

    def _load(self, dataset_id: str, graph_path: Path, checksum: str) -> GraphArrays:
        out = self.s.cache_dir / dataset_id / checksum
        meta_file = out / "export.json"
        if not meta_file.exists():
            tmp = out.with_name(out.name + ".tmp")
            shutil.rmtree(tmp, ignore_errors=True)
            # Fixed argv of registry-derived paths; no request data reaches it.
            proc = subprocess.run(  # noqa: S603
                [
                    str(self.s.tool("lpq_export")),
                    "--dataset",
                    str(graph_path),
                    "--out-dir",
                    str(tmp),
                ],
                capture_output=True,
                text=True,
                timeout=600,
                check=False,
            )
            if proc.returncode != 0:
                raise GraphUnavailable("the engine could not export this graph")
            shutil.rmtree(out, ignore_errors=True)
            tmp.rename(out)
        meta = json.loads(meta_file.read_text())
        if meta["checksum_fnv1a64"] != checksum:
            raise GraphUnavailable("exported arrays do not match the graph checksum")
        n = int(meta["nodes"])
        coords = None
        if meta.get("has_coordinates"):
            coords = np.fromfile(out / "coords.f64", dtype="<f8").reshape(n, 2)
        osm_ids = None
        osm_file = self.s.root / "datasets" / "processed" / f"{dataset_id}.osm_node_ids.u64"
        if osm_file.exists():
            ids = np.fromfile(osm_file, dtype="<u8")
            osm_ids = ids if len(ids) == n else None
        return GraphArrays(
            dataset_id=dataset_id,
            checksum=checksum,
            nodes=n,
            directed=bool(meta["directed"]),
            coordinate_system=meta["coordinate_system"],
            coords=coords,
            offsets=np.fromfile(out / "offsets.u64", dtype="<u8"),
            targets=np.fromfile(out / "targets.u32", dtype="<u4"),
            weights=np.fromfile(out / "weights.u32", dtype="<u4"),
            osm_ids=osm_ids,
        )


def snap(g: GraphArrays, lon: float, lat: float) -> dict[str, object]:
    """Nearest graph node to a clicked point (equirectangular search, haversine report)."""
    if g.coords is None or g.coordinate_system != "geographic":
        raise GraphUnavailable("this dataset has no geographic coordinates to snap to")
    k = math.cos(math.radians(lat))
    dx = (g.coords[:, 0] - lon) * k
    dy = g.coords[:, 1] - lat
    i = int(np.argmin(dx * dx + dy * dy))
    nlon, nlat = float(g.coords[i, 0]), float(g.coords[i, 1])
    out: dict[str, object] = {
        "node": i,
        "lon": nlon,
        "lat": nlat,
        "clicked": {"lon": lon, "lat": lat},
        "snap_distance_m": haversine_m(lon, lat, nlon, nlat),
        "out_degree": int(g.offsets[i + 1] - g.offsets[i]),
    }
    if g.osm_ids is not None:
        out["osm_node_id"] = int(g.osm_ids[i])
    return out


def viewport(
    g: GraphArrays, bbox: tuple[float, float, float, float], limit: int
) -> dict[str, object]:
    """Road segments with an endpoint inside ``bbox`` as flat [lon1, lat1, lon2, lat2] rows."""
    if g.coords is None:
        raise GraphUnavailable("this dataset has no coordinates to draw")
    w, s, e, n = bbox
    inside = (
        (g.coords[:, 0] >= w)
        & (g.coords[:, 0] <= e)
        & (g.coords[:, 1] >= s)
        & (g.coords[:, 1] <= n)
    )
    src = g.arc_sources
    dst = g.targets.astype(np.int64)
    sel = inside[src] | inside[dst]
    a, b = src[sel], dst[sel]
    # One segment per undirected road piece: a one-way road appears once, a
    # two-way road's two arcs collapse to one line.
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    key = np.unique(lo * g.nodes + hi)
    count = len(key)
    if count > limit:
        return {"segments": [], "count": count, "too_many": True, "limit": limit}
    lo, hi = key // g.nodes, key % g.nodes
    seg = np.column_stack((g.coords[lo], g.coords[hi]))
    return {
        "segments": np.round(seg, 7).tolist(),
        "count": count,
        "too_many": False,
        "limit": limit,
    }


def node_coords(g: GraphArrays, ids: list[int]) -> dict[str, list[float]]:
    if g.coords is None:
        raise GraphUnavailable("this dataset has no coordinates")
    out: dict[str, list[float]] = {}
    for i in ids:
        if 0 <= i < g.nodes:
            out[str(i)] = [float(g.coords[i, 0]), float(g.coords[i, 1])]
    return out
