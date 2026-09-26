"""Fixture repository for API tests.

A throwaway repository root holding one small graph, converted by the REAL
lpq_convert, with two strongly connected parts that cannot reach each other
(so unreachable queries are genuine) and geographic coordinates (so A* runs).
The API is pointed at it through ATLAS_REPO_ROOT, and at the real engine
binaries through the LPQ_* tool overrides RepoPaths already supports.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
BIN = REPO / "build" / "release" / "bin"
EXE = ".exe" if os.name == "nt" else ""


def _tool(name: str) -> Path:
    p = BIN / f"{name}{EXE}"
    if not p.exists():
        pytest.skip(f"{name} is not built; build the release preset first", allow_module_level=True)
    return p


def _grid_component(offset: int, rows: int, cols: int, lon0: float) -> tuple[list[str], list[str]]:
    arcs, coords = [], []
    for r in range(rows):
        for c in range(cols):
            u = offset + r * cols + c + 1
            coords.append(
                f"v {u} {round((lon0 + c * 0.001) * 1e6)} {round((43.7 + r * 0.001) * 1e6)}"
            )
            for dr, dc in ((0, 1), (1, 0)):
                rr, cc = r + dr, c + dc
                if rr < rows and cc < cols:
                    v = offset + rr * cols + cc + 1
                    w = 800 + ((u * 7 + v * 3) % 400)  # decimetres, >= the geometric length
                    arcs += [f"a {u} {v} {w}", f"a {v} {u} {w}"]
    return arcs, coords


FIXTURE_ID = "fx_twoparts"
ROWS, COLS = 8, 8
PART = ROWS * COLS


@pytest.fixture(scope="session")
def fixture_root(tmp_path_factory: pytest.TempPathFactory) -> Path:
    root = tmp_path_factory.mktemp("atlasrepo")
    for d in (
        "datasets/metadata",
        "datasets/processed",
        "configs/datasets",
        "experiments/manifests",
        "models/exported",
        "results/raw",
        "docs/experiments",
        "experiments/paper",
    ):
        (root / d).mkdir(parents=True, exist_ok=True)
    a1, c1 = _grid_component(0, ROWS, COLS, 7.40)
    a2, c2 = _grid_component(PART, ROWS, COLS, 7.45)
    arcs, coords = a1 + a2, c1 + c2
    n = 2 * PART
    stage = root / "stage"
    stage.mkdir()
    (stage / "g.gr").write_text(f"p sp {n} {len(arcs)}\n" + "\n".join(arcs) + "\n")
    (stage / "g.co").write_text(f"p aux sp co {n}\n" + "\n".join(coords) + "\n")
    subprocess.run(
        [
            str(_tool("lpq_convert")),
            "--input",
            str(stage / "g.gr"),
            "--format",
            "dimacs",
            "--coordinates",
            str(stage / "g.co"),
            "--output",
            str(root / "datasets" / "processed" / f"{FIXTURE_ID}.lpqgraph"),
            "--metadata",
            str(root / "datasets" / "metadata" / f"{FIXTURE_ID}.json"),
            "--dataset-id",
            FIXTURE_ID,
            "--directedness",
            "directed",
        ],
        check=True,
        capture_output=True,
    )
    return root


@pytest.fixture(scope="session")
def client(fixture_root: Path, tmp_path_factory: pytest.TempPathFactory):  # type: ignore[no-untyped-def]
    os.environ["ATLAS_REPO_ROOT"] = str(fixture_root)
    os.environ["ATLAS_WORK_DIR"] = str(tmp_path_factory.mktemp("atlaswork"))
    for name in ("lpq_run", "lpq_benchmark", "lpq_export", "lpq_model_info", "lpq_convert"):
        os.environ[name.upper()] = str(_tool(name))
    sys.path.insert(0, str(REPO / "atlas" / "api"))
    from atlas_api.app import app
    from fastapi.testclient import TestClient

    with TestClient(app) as c:
        yield c
