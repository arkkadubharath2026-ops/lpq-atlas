"""OpenStreetMap road-network extraction (LPQ Atlas spec D-01, sections 10 and 54).

Turns a pinned ``.osm.pbf`` regional snapshot into the DIMACS ``.gr``/``.co``
pair that ``lpq_convert`` already reads, so an OSM graph enters the project
through the same converter, checksum and metadata path as every other dataset.
There is no second graph engine and no second binary writer.

Road-access policy (version ``ROAD_POLICY_VERSION``), stated here because a
route is only meaningful relative to it:

* **Drivable ways** are ``highway`` values in :data:`DRIVABLE_HIGHWAYS`. Paths,
  footways, cycleways, steps, tracks, bridleways, platforms and anything under
  construction or proposed are excluded.
* **Access**: a way whose ``access``, ``vehicle``, ``motor_vehicle`` or
  ``motorcar`` tag is ``no`` or ``private`` is excluded. ``service`` ways tagged
  ``parking_aisle`` or ``driveway`` are excluded; other service roads are kept.
  ``area=yes`` ways are excluded (a pedestrian plaza is not a road segment).
* **One-way**: ``oneway=yes|true|1`` is forward only, ``oneway=-1|reverse`` is
  backward only, ``oneway=no`` is both ways. Without an explicit tag,
  ``junction=roundabout|circular`` and ``highway=motorway|motorway_link`` are
  one-way forward, as the OSM wiki specifies; everything else is two-way.
* **Bridges and tunnels** need no special handling: topology comes only from
  shared OSM nodes, so a bridge over a road never connects to it.
* **Turn restrictions are NOT modelled.** Relations of type ``restriction`` are
  ignored, so a returned route may use a turn that is prohibited on the ground.
* **Missing coordinates**: a segment touching a node without a valid location
  is dropped and counted.
* **Disconnected components**: only the largest strongly connected component
  is kept, so every source/target pair in the graph is mutually reachable. The
  number of nodes dropped is recorded.
* **Duplicate / parallel ways** are kept as parallel arcs (``parallel_edges:
  keep`` in the dataset config); Dijkstra uses the cheapest.
* **Geometry**: every OSM way node becomes a graph node (no contraction of
  degree-2 nodes), so the route drawn on the map follows the road exactly.

Edge weights are **great-circle length in decimetres** (rounded, minimum 1).
They are distances: a returned route is the *shortest*, never the *fastest*.
"""

from __future__ import annotations

import math
from itertools import pairwise
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

#: Bumped whenever the policy above changes what a conversion produces.
ROAD_POLICY_VERSION = 1

#: Units of the integer edge weight.
WEIGHT_UNIT = "decimetre"
WEIGHT_PER_METRE = 10.0

DRIVABLE_HIGHWAYS = frozenset(
    {
        "motorway",
        "motorway_link",
        "trunk",
        "trunk_link",
        "primary",
        "primary_link",
        "secondary",
        "secondary_link",
        "tertiary",
        "tertiary_link",
        "unclassified",
        "residential",
        "living_street",
        "service",
        "road",
    }
)
_DENIED_ACCESS = frozenset({"no", "private"})
_ACCESS_KEYS = ("access", "vehicle", "motor_vehicle", "motorcar")
_EXCLUDED_SERVICE = frozenset({"parking_aisle", "driveway"})
_IMPLIED_ONEWAY_HIGHWAY = frozenset({"motorway", "motorway_link"})
_IMPLIED_ONEWAY_JUNCTION = frozenset({"roundabout", "circular"})

_EARTH_RADIUS_M = 6371008.8


class OsmExtractionError(RuntimeError):
    """Raised when a PBF yields no usable road graph."""


@dataclass
class OsmExtractReport:
    """What an extraction kept and dropped. Written into the dataset metadata."""

    road_policy_version: int = ROAD_POLICY_VERSION
    weight_unit: str = WEIGHT_UNIT
    ways_seen: int = 0
    ways_kept: int = 0
    ways_excluded: dict[str, int] = field(default_factory=dict)
    segments_missing_location: int = 0
    nodes_before_scc: int = 0
    arcs_before_scc: int = 0
    nodes_kept: int = 0
    arcs_kept: int = 0
    nodes_dropped_outside_largest_scc: int = 0
    turn_restrictions_modelled: bool = False
    bbox: list[float] = field(default_factory=list)  # [min_lon, min_lat, max_lon, max_lat]

    def as_dict(self) -> dict[str, Any]:
        return dict(self.__dict__)


def classify_way(tags: dict[str, str]) -> tuple[str | None, int]:
    """Decides whether a way is a road and in which direction it may be driven.

    Returns ``(exclusion_reason, direction)``. ``exclusion_reason`` is None for a
    kept way. ``direction`` is +1 (forward only), -1 (backward only) or 0 (both).
    Pure so the policy can be tested without a PBF.
    """
    highway = tags.get("highway")
    if highway is None:
        return "not_highway", 0
    if highway not in DRIVABLE_HIGHWAYS:
        return f"highway={highway}", 0
    if tags.get("area") == "yes":
        return "area", 0
    for key in _ACCESS_KEYS:
        if tags.get(key) in _DENIED_ACCESS:
            return f"{key}={tags[key]}", 0
    if highway == "service" and tags.get("service") in _EXCLUDED_SERVICE:
        return f"service={tags['service']}", 0

    oneway = tags.get("oneway")
    if oneway in ("yes", "true", "1"):
        return None, 1
    if oneway in ("-1", "reverse"):
        return None, -1
    if oneway == "no":
        return None, 0
    if highway in _IMPLIED_ONEWAY_HIGHWAY or tags.get("junction") in _IMPLIED_ONEWAY_JUNCTION:
        return None, 1
    return None, 0


def haversine_m(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    """Great-circle distance in metres (same sphere radius as heuristics.hpp)."""
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    s_dlat = math.sin((p2 - p1) * 0.5)
    s_dlon = math.sin(math.radians(lon2 - lon1) * 0.5)
    h = s_dlat * s_dlat + math.cos(p1) * math.cos(p2) * s_dlon * s_dlon
    return 2.0 * math.asin(math.sqrt(min(1.0, max(0.0, h)))) * _EARTH_RADIUS_M


def _collect_segments(
    pbf: Path, report: OsmExtractReport
) -> tuple[dict[int, tuple[float, float]], list[tuple[int, int, int]]]:
    import osmium  # imported lazily: only dataset preparation needs it

    locations: dict[int, tuple[float, float]] = {}
    arcs: list[tuple[int, int, int]] = []

    class _Handler(osmium.SimpleHandler):  # type: ignore[misc]
        def way(self, w: Any) -> None:
            tags = {t.k: t.v for t in w.tags}
            if "highway" not in tags:
                return
            report.ways_seen += 1
            reason, direction = classify_way(tags)
            if reason is not None:
                report.ways_excluded[reason] = report.ways_excluded.get(reason, 0) + 1
                return
            report.ways_kept += 1
            refs = []
            for n in w.nodes:
                if not n.location.valid():
                    refs.append(None)
                    continue
                locations[n.ref] = (n.location.lon, n.location.lat)
                refs.append(n.ref)
            for a, b in pairwise(refs):
                if a is None or b is None:
                    report.segments_missing_location += 1
                    continue
                if a == b:
                    continue
                (x1, y1), (x2, y2) = locations[a], locations[b]
                weight = max(1, round(haversine_m(x1, y1, x2, y2) * WEIGHT_PER_METRE))
                if direction >= 0:
                    arcs.append((a, b, weight))
                if direction <= 0:
                    arcs.append((b, a, weight))

    _Handler().apply_file(str(pbf), locations=True)
    return locations, arcs


def _largest_scc(n: int, src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    from scipy.sparse import csr_matrix
    from scipy.sparse.csgraph import connected_components

    m = csr_matrix((np.ones(len(src), dtype=np.int8), (src, dst)), shape=(n, n))
    _, labels = connected_components(m, directed=True, connection="strong")
    largest = np.bincount(labels).argmax()
    return labels == largest


def extract_road_graph(pbf: Path, out_stem: Path) -> tuple[Path, Path, Path, OsmExtractReport]:
    """Extracts the drivable road graph from ``pbf``.

    Writes ``<out_stem>.gr`` and ``<out_stem>.co`` (DIMACS, 1-based ids assigned
    in ascending OSM node id order, so the mapping is deterministic) and
    ``<out_stem>.osm_node_ids.u64`` holding each graph node's OSM node id, where
    graph node ``i`` (0-based, as in the converted graph) is DIMACS id ``i + 1``.
    """
    report = OsmExtractReport()
    locations, arcs = _collect_segments(pbf, report)
    if not arcs:
        raise OsmExtractionError(f"{pbf.name}: no drivable road segments found")

    osm_ids = np.array(sorted({a for a, _, _ in arcs} | {b for _, b, _ in arcs}), dtype=np.int64)
    index = {int(v): i for i, v in enumerate(osm_ids)}
    src = np.fromiter((index[a] for a, _, _ in arcs), dtype=np.int64, count=len(arcs))
    dst = np.fromiter((index[b] for _, b, _ in arcs), dtype=np.int64, count=len(arcs))
    wgt = np.fromiter((w for _, _, w in arcs), dtype=np.int64, count=len(arcs))
    report.nodes_before_scc = len(osm_ids)
    report.arcs_before_scc = len(arcs)

    keep = _largest_scc(len(osm_ids), src, dst)
    kept_ids = osm_ids[keep]
    new_index = np.full(len(osm_ids), -1, dtype=np.int64)
    new_index[keep] = np.arange(int(keep.sum()))
    arc_keep = keep[src] & keep[dst]
    src2 = new_index[src[arc_keep]]
    dst2 = new_index[dst[arc_keep]]
    wgt2 = wgt[arc_keep]
    report.nodes_kept = len(kept_ids)
    report.arcs_kept = int(arc_keep.sum())
    report.nodes_dropped_outside_largest_scc = report.nodes_before_scc - report.nodes_kept
    if report.nodes_kept < 2:
        raise OsmExtractionError(f"{pbf.name}: largest strongly connected component is trivial")

    lon = np.array([locations[int(i)][0] for i in kept_ids])
    lat = np.array([locations[int(i)][1] for i in kept_ids])
    report.bbox = [float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max())]

    gr = out_stem.with_suffix(".gr")
    co = out_stem.with_suffix(".co")
    ids_path = out_stem.with_suffix(".osm_node_ids.u64")
    out_stem.parent.mkdir(parents=True, exist_ok=True)
    order = np.lexsort((dst2, src2))  # deterministic arc order
    with gr.open("w", encoding="ascii", newline="\n") as f:
        f.write(f"c OSM road graph from {pbf.name}, road policy v{ROAD_POLICY_VERSION}\n")
        f.write(f"c weights: great-circle {WEIGHT_UNIT}s\n")
        f.write(f"p sp {report.nodes_kept} {report.arcs_kept}\n")
        f.writelines(
            f"a {s + 1} {d + 1} {w}\n" for s, d, w in zip(src2[order], dst2[order], wgt2[order], strict=True)
        )
    with co.open("w", encoding="ascii", newline="\n") as f:
        f.write(f"c coordinates of {pbf.name} in microdegrees (lon, lat)\n")
        f.write(f"p aux sp co {report.nodes_kept}\n")
        f.writelines(
            f"v {i + 1} {round(x * 1e6)} {round(y * 1e6)}\n"
            for i, (x, y) in enumerate(zip(lon, lat, strict=True))
        )
    kept_ids.astype("<u8").tofile(ids_path)
    return gr, co, ids_path, report
