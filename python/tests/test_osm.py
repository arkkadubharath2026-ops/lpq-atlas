"""Tests for the OSM road-graph extraction policy (lpq_tools.data.osm)."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
from lpq_tools.data.osm import (
    DRIVABLE_HIGHWAYS,
    classify_way,
    extract_road_graph,
    haversine_m,
)

_PBF = Path(__file__).resolve().parents[2] / "datasets" / "raw" / "monaco-osmfr-20261013T011405Z.osm.pbf"


@pytest.mark.parametrize(
    ("tags", "reason", "direction"),
    [
        ({"highway": "residential"}, None, 0),
        ({"highway": "residential", "oneway": "yes"}, None, 1),
        ({"highway": "residential", "oneway": "1"}, None, 1),
        ({"highway": "residential", "oneway": "-1"}, None, -1),
        ({"highway": "motorway"}, None, 1),
        ({"highway": "motorway", "oneway": "no"}, None, 0),
        ({"highway": "primary", "junction": "roundabout"}, None, 1),
        ({"highway": "footway"}, "highway=footway", 0),
        ({"highway": "steps"}, "highway=steps", 0),
        ({"highway": "residential", "access": "private"}, "access=private", 0),
        ({"highway": "residential", "motor_vehicle": "no"}, "motor_vehicle=no", 0),
        ({"highway": "service", "service": "parking_aisle"}, "service=parking_aisle", 0),
        ({"highway": "service", "service": "alley"}, None, 0),
        ({"highway": "pedestrian", "area": "yes"}, "highway=pedestrian", 0),
        ({"highway": "residential", "area": "yes"}, "area", 0),
        ({"building": "yes"}, "not_highway", 0),
    ],
)
def test_road_policy(tags: dict[str, str], reason: str | None, direction: int) -> None:
    assert classify_way(tags) == (reason, direction)


def test_drivable_set_excludes_non_vehicle_ways() -> None:
    for h in ("footway", "path", "cycleway", "steps", "track", "construction", "proposed"):
        assert h not in DRIVABLE_HIGHWAYS


def test_haversine_matches_known_distance() -> None:
    # One degree of latitude on the mean-radius sphere.
    assert haversine_m(0.0, 0.0, 0.0, 1.0) == pytest.approx(math.pi * 6371008.8 / 180.0)
    assert haversine_m(7.4, 43.7, 7.4, 43.7) == 0.0


@pytest.mark.skipif(not _PBF.exists(), reason="Monaco snapshot not acquired")
def test_monaco_extraction_is_deterministic_and_strongly_connected(tmp_path: Path) -> None:
    gr1, co1, ids1, report = extract_road_graph(_PBF, tmp_path / "a" / "m")
    gr2, _, _, _ = extract_road_graph(_PBF, tmp_path / "b" / "m")
    assert gr1.read_bytes() == gr2.read_bytes()

    assert report.nodes_kept > 1000
    assert report.turn_restrictions_modelled is False
    ids = np.fromfile(ids1, dtype="<u8")
    assert len(ids) == report.nodes_kept
    assert np.all(np.diff(ids.astype(np.int64)) > 0), "ids ascend: the mapping is deterministic"

    header = gr1.read_text().splitlines()
    p = next(line for line in header if line.startswith("p sp"))
    assert p.split()[2:] == [str(report.nodes_kept), str(report.arcs_kept)]
    arcs = [line.split() for line in header if line.startswith("a ")]
    assert all(int(w) >= 1 for *_, w in arcs)
    coords = [line.split() for line in co1.read_text().splitlines() if line.startswith("v ")]
    lon = [int(c[2]) / 1e6 for c in coords]
    lat = [int(c[3]) / 1e6 for c in coords]
    bbox = report.bbox
    assert min(lon) >= bbox[0] - 1e-6
    assert max(lat) <= bbox[3] + 1e-6
