"""Tests for derived (same-topology, different-weights) datasets.

These run the REAL ``lpq_reweight`` against small graphs converted by the REAL
``lpq_convert``, and recompute the weight statistics independently of both: the
graph is dumped by ``lpq_export`` (the one reader of the format) and the numbers
are recomputed with numpy. A metadata record checked against the same C++ code
that wrote it would prove nothing.

They skip, with the build command, when a tool has not been built. Point
``LPQ_REWEIGHT`` at an executable built outside ``build/release`` if needed.

The last group reads the COMMITTED fixed set (``configs/derived_datasets.yaml``)
and checks its labelling and manifests; it needs no executable.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np
import pytest
import yaml

from lpq_tools.data.binary_header import read_binary_graph_header
from lpq_tools.data.derive import (
    DeriveError,
    derive_all,
    derive_dataset,
    parse_transform,
    read_derived_manifest,
)
from lpq_tools.data.queries import build_manifest, read_manifest, write_manifest
from lpq_tools.utilities.jsonio import read_json
from lpq_tools.utilities.paths import RepoPaths

REPO = RepoPaths.discover()


def _find(name: str) -> Path:
    try:
        return REPO.tool(name)
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


@pytest.fixture(scope="module")
def tools():
    return {name: _find(name) for name in ("lpq_convert", "lpq_reweight", "lpq_export")}


# -- a small base graph ---------------------------------------------------------

N = 40


def _dimacs_text() -> tuple[str, str]:
    """A ring with chords and one deliberate parallel arc pair, both directions."""
    arcs: list[tuple[int, int, int]] = []
    for i in range(N):
        for j in ((i + 1) % N, (i + 7) % N):
            w = 1 + (i * 7 + j) % 13
            arcs.append((i, j, w))
            arcs.append((j, i, w))
    arcs.append((0, 1, 50))  # parallel to ring edge 0-1, with a different weight
    arcs.append((1, 0, 50))
    gr = [f"p sp {N} {len(arcs)}"] + [f"a {u + 1} {v + 1} {w}" for u, v, w in arcs]
    co = [f"p aux sp co {N}"] + [
        f"v {i + 1} {-74_000_000 + 1000 * i} {40_000_000 + 700 * (i % 5)}" for i in range(N)
    ]
    return "\n".join(gr) + "\n", "\n".join(co) + "\n"


def _snap_text() -> str:
    lines = []
    for i in range(N):
        lines.append(f"{i} {(i + 1) % N}")
        lines.append(f"{i} {(i + 11) % N}")
    return "\n".join(lines) + "\n"


def _convert(tools, workspace: RepoPaths, dataset_id: str, fmt: str) -> None:
    raw = workspace.staging / f"{dataset_id}.txt"
    args = [str(tools["lpq_convert"]), "--format", fmt, "--input", str(raw)]
    if fmt == "dimacs":
        gr, co = _dimacs_text()
        raw.write_text(gr, encoding="utf-8")
        co_path = workspace.staging / f"{dataset_id}.co"
        co_path.write_text(co, encoding="utf-8")
        args += ["--coordinates", str(co_path)]
    else:
        raw.write_text(_snap_text(), encoding="utf-8")
    graph = workspace.processed / f"{dataset_id}.lpqgraph"
    meta = workspace.metadata / f"{dataset_id}.json"
    args += ["--output", str(graph), "--metadata", str(meta), "--dataset-id", dataset_id]
    subprocess.run(args, check=True, capture_output=True, text=True)
    header = read_binary_graph_header(graph)
    for suffix, count in (("queries", 100), ("queries_first30", 30)):
        write_manifest(
            workspace.manifests / f"{dataset_id}_{suffix}.yaml",
            build_manifest(
                dataset_id, header.node_count, count=count, seed=42,
                graph_checksum=header.checksum,
            ),
        )


@pytest.fixture
def workspace(tmp_path, monkeypatch, tools):
    for marker in ("PRD.md", "ESD.md", "CMakePresets.json"):
        (tmp_path / marker).write_text("test fixture\n", encoding="utf-8")
    monkeypatch.setenv("LPQ_REWEIGHT", str(tools["lpq_reweight"]))
    paths = RepoPaths(tmp_path)
    paths.ensure_directories()
    _convert(tools, paths, "toy_road", "dimacs")  # directed, with coordinates
    _convert(tools, paths, "toy_social", "snap")  # undirected, no coordinates
    return paths


def _export(tools, graph: Path, out: Path) -> dict[str, np.ndarray]:
    subprocess.run(
        [str(tools["lpq_export"]), "--dataset", str(graph), "--out-dir", str(out)],
        check=True, capture_output=True, text=True,
    )
    arrays = {
        "offsets": np.fromfile(out / "offsets.u64", dtype="<u8"),
        "targets": np.fromfile(out / "targets.u32", dtype="<u4"),
        "weights": np.fromfile(out / "weights.u32", dtype="<u4"),
    }
    if (out / "coords.f64").exists():
        arrays["coords"] = np.fromfile(out / "coords.f64", dtype="<f8")
    return arrays


# -- parsing --------------------------------------------------------------------


def test_parse_transform_shapes():
    assert parse_transform("unit").params == ()
    assert parse_transform("uniform:1:16").parameters() == {"minimum": 1.0, "maximum": 16.0}
    assert parse_transform("bimodal:10:30000:0.02").parameters()["p_large"] == 0.02
    for bad in ("nope", "uniform:1", "scale", "unit:3", "lognormal:a:b"):
        with pytest.raises(DeriveError):
            parse_transform(bad)


# -- determinism, refusal, labelling --------------------------------------------


@pytest.mark.parametrize("spec", ["uniform:1:9", "lognormal:5:0.8", "pareto:2:1.5",
                                  "bimodal:1:100:0.1"])
def test_same_seed_same_graph_different_seed_different_graph(workspace, spec):
    a = derive_dataset("toy_road", spec, 7, "derived_a", paths=workspace)
    b = derive_dataset("toy_road", spec, 7, "derived_b", paths=workspace)
    c = derive_dataset("toy_road", spec, 8, "derived_c", paths=workspace)
    assert a.checksum == b.checksum
    assert a.graph_path.read_bytes() == b.graph_path.read_bytes()
    assert a.checksum != c.checksum


def test_refuses_to_overwrite_and_leaves_existing_files_untouched(workspace):
    first = derive_dataset("toy_road", "uniform:1:9", 1, "derived_x", paths=workspace)
    before = {p: p.read_bytes() for p in
              (first.graph_path, first.metadata_path, *first.manifest_paths)}
    with pytest.raises(DeriveError, match="refusing to overwrite"):
        derive_dataset("toy_road", "uniform:1:9", 2, "derived_x", paths=workspace)
    assert {p: p.read_bytes() for p in before} == before


def test_the_tool_itself_refuses_to_overwrite(workspace, tools):
    out = workspace.processed / "direct.lpqgraph"
    cmd = [str(tools["lpq_reweight"]), "--input", str(workspace.processed / "toy_road.lpqgraph"),
           "--output", str(out), "--transform", "unit", "--seed", "1"]
    assert subprocess.run(cmd, capture_output=True, text=True).returncode == 0
    again = subprocess.run(cmd, capture_output=True, text=True)
    assert again.returncode != 0
    assert "refusing to overwrite" in again.stderr


def test_ids_must_be_labelled_derived_and_bases_must_be_real(workspace):
    with pytest.raises(DeriveError, match="derived_"):
        derive_dataset("toy_road", "unit", 1, "toy_unit", paths=workspace)
    derive_dataset("toy_road", "unit", 1, "derived_u", paths=workspace)
    with pytest.raises(DeriveError, match="derived dataset"):
        derive_dataset("derived_u", "unit", 1, "derived_v", paths=workspace)
    with pytest.raises(DeriveError, match="does not exist"):
        derive_dataset("no_such_base", "unit", 1, "derived_w", paths=workspace)


def test_a_failed_derivation_leaves_nothing_behind(workspace):
    with pytest.raises(DeriveError, match="lpq_reweight failed"):
        derive_dataset("toy_road", "uniform:5:2", 1, "derived_bad", paths=workspace)
    leftovers = [p.name for d in (workspace.processed, workspace.metadata, workspace.manifests)
                 for p in d.iterdir() if "derived_bad" in p.name]
    assert leftovers == []
    assert not (workspace.root / "configs" / "datasets" / "derived" / "derived_bad.yaml").exists()


def test_metadata_is_labelled_synthetic_with_full_provenance(workspace):
    result = derive_dataset("toy_road", "lognormal:5:0.8", 31, "derived_lab", paths=workspace)
    meta = read_json(result.metadata_path)
    base = read_json(workspace.metadata / "toy_road.json")
    assert meta["dataset_id"] == "derived_lab"
    assert meta["derived"] is True
    assert meta["derived_from"] == "toy_road"
    assert meta["transform"] == "lognormal:5:0.8"
    assert meta["seed"] == 31
    assert meta["base_graph_checksum_fnv1a64"] == base["graph_checksum_fnv1a64"]
    w = meta["weights"]
    assert w["synthesized"] is True
    assert (w["derived_from"], w["transform"], w["seed"]) == ("toy_road", "lognormal:5:0.8", 31)
    assert "not a real road network" in meta["description"]
    # Same schema as the converter's records: every key lpq_convert writes is present.
    assert set(base) - set(meta) == set()


# -- metadata against an independent recomputation ------------------------------


@pytest.mark.parametrize("base,spec", [
    ("toy_road", "uniform:1:9"), ("toy_road", "lognormal:5:0.8"),
    ("toy_road", "pareto:2:1.5"), ("toy_social", "bimodal:1:100:0.1"),
    ("toy_social", "scale:4"), ("toy_social", "clip:3"),
])
def test_metadata_matches_independent_recomputation(workspace, tools, tmp_path, base, spec):
    result = derive_dataset(base, spec, 3, "derived_chk", paths=workspace)
    meta = read_json(result.metadata_path)
    arrays = _export(tools, result.graph_path, tmp_path / "exp")
    w = arrays["weights"].astype(np.float64)
    n = len(arrays["offsets"]) - 1
    degrees = np.diff(arrays["offsets"].astype(np.int64))

    header = read_binary_graph_header(result.graph_path)
    assert meta["graph_checksum_fnv1a64"] == header.checksum
    assert meta["node_count"] == n == header.node_count
    assert meta["arc_count"] == len(w) == header.arc_count
    assert meta["edge_count"] == header.edge_count
    assert meta["directed"] == header.directed
    assert meta["min_weight"] == w.min()
    assert meta["max_weight"] == w.max()
    assert meta["mean_weight"] == pytest.approx(w.mean(), rel=1e-12)
    assert meta["stddev_weight"] == pytest.approx(w.std(), rel=1e-9)
    assert meta["max_path_cost_bound"] == pytest.approx((n - 1) * w.max(), rel=1e-12)
    assert meta["min_out_degree"] == degrees.min()
    assert meta["max_out_degree"] == degrees.max()
    assert meta["mean_out_degree"] == pytest.approx(degrees.mean(), rel=1e-12)
    assert meta["isolated_nodes"] == int((degrees == 0).sum())
    assert w.min() >= 1 and w.max() <= 2**31 - 1


@pytest.mark.parametrize("base", ["toy_road", "toy_social"])
def test_topology_coordinates_and_symmetry_are_preserved(workspace, tools, tmp_path, base):
    result = derive_dataset(base, "uniform:1:1000", 4, "derived_top", paths=workspace)
    old = _export(tools, workspace.processed / f"{base}.lpqgraph", tmp_path / "old")
    new = _export(tools, result.graph_path, tmp_path / "new")
    assert np.array_equal(old["offsets"], new["offsets"])
    assert np.array_equal(old["targets"], new["targets"])
    assert ("coords" in old) == ("coords" in new)
    if "coords" in old:
        assert np.array_equal(old["coords"], new["coords"])
    assert (read_binary_graph_header(result.graph_path).coordinate_system
            == read_binary_graph_header(workspace.processed / f"{base}.lpqgraph").coordinate_system)

    # Both directions of every edge carry the same weight (multiset per ordered
    # pair, so the deliberate parallel arc pair is covered too).
    n = len(new["offsets"]) - 1
    sources = np.repeat(np.arange(n), np.diff(new["offsets"].astype(np.int64)))
    forward: dict[tuple[int, int], list[int]] = {}
    for u, v, w in zip(sources, new["targets"], new["weights"]):
        forward.setdefault((int(u), int(v)), []).append(int(w))
    for (u, v), ws in forward.items():
        assert sorted(ws) == sorted(forward[(v, u)])


def test_deterministic_transforms_compute_what_they_say(workspace, tools, tmp_path):
    base = _export(tools, workspace.processed / "toy_road.lpqgraph", tmp_path / "b")["weights"]
    unit = derive_dataset("toy_road", "unit", 1, "derived_t1", paths=workspace)
    scale = derive_dataset("toy_road", "scale:3", 1, "derived_t2", paths=workspace)
    clip = derive_dataset("toy_road", "clip:5", 1, "derived_t3", paths=workspace)
    bimodal = derive_dataset("toy_road", "bimodal:2:77:0.5", 1, "derived_t4", paths=workspace)
    got = {r.dataset_id: _export(tools, r.graph_path, tmp_path / r.dataset_id)["weights"]
           for r in (unit, scale, clip, bimodal)}
    # Weights within an adjacency list are re-sorted by (target, weight), so
    # compare as sorted multisets rather than positionally.
    assert set(got["derived_t1"]) == {1}
    assert sorted(got["derived_t2"]) == sorted(3 * base)
    assert sorted(got["derived_t3"]) == sorted(np.minimum(base, 5))
    assert set(got["derived_t4"]) == {2, 77}


def test_clamping_is_counted_not_silent(workspace):
    # alpha this small makes almost every draw exceed the weight ceiling.
    result = derive_dataset("toy_road", "pareto:1:0.05", 1, "derived_clamp", paths=workspace)
    meta = read_json(result.metadata_path)
    assert meta["max_weight"] == 2**31 - 1
    assert meta["weights"]["clamped_high_draws"] > 0


# -- query manifests ------------------------------------------------------------


def test_manifests_reuse_base_endpoints_with_the_new_checksum(workspace):
    result = derive_dataset("toy_road", "uniform:1:9", 1, "derived_man", paths=workspace)
    names = sorted(p.name for p in result.manifest_paths)
    assert names == ["derived_man_queries.yaml", "derived_man_queries_first30.yaml"]
    for suffix in ("queries", "queries_first30"):
        base = read_manifest(workspace.manifests / f"toy_road_{suffix}.yaml")
        new = read_manifest(workspace.manifests / f"derived_man_{suffix}.yaml")
        assert new.dataset_id == "derived_man"
        assert new.pairs == base.pairs
        assert (new.node_count, new.seed) == (base.node_count, base.seed)
        assert new.graph_checksum == result.checksum != base.graph_checksum


# -- the fixed set --------------------------------------------------------------


def test_derive_all_builds_missing_pins_and_detects_drift(workspace, tmp_path):
    manifest = tmp_path / "derived.yaml"
    manifest.write_text(yaml.safe_dump({"datasets": [
        {"id": "derived_s1", "base": "toy_road", "transform": "uniform:1:9", "seed": 11},
        {"id": "derived_s2", "base": "toy_social", "transform": "pareto:2:1.5", "seed": 12},
    ]}), encoding="utf-8")
    assert derive_all(manifest, paths=workspace, pin=True) == []
    entries = {e.dataset_id: e for e in read_derived_manifest(manifest)}
    assert all(e.pinned_checksum for e in entries.values())
    stored = yaml.safe_load(manifest.read_text(encoding="utf-8"))["datasets"][0]
    assert stored["node_count"] == N and stored["max_weight"] <= 9

    assert derive_all(manifest, paths=workspace) == []  # verifies, rebuilds nothing

    raw = yaml.safe_load(manifest.read_text(encoding="utf-8"))
    raw["datasets"][0]["graph_checksum_fnv1a64"] += 1
    manifest.write_text(yaml.safe_dump(raw), encoding="utf-8")
    problems = derive_all(manifest, paths=workspace)
    assert len(problems) == 1 and "derived_s1" in problems[0]


# -- the committed fixed set (no executables needed) ----------------------------

MANIFEST = REPO.root / "configs" / "derived_datasets.yaml"


def test_committed_fixed_set_is_labelled_pinned_and_consistent():
    entries = read_derived_manifest(MANIFEST)
    assert len(entries) == len({e.dataset_id for e in entries}) >= 12
    seeds = [(e.base, e.seed) for e in entries]
    assert len(seeds) == len(set(seeds)), "seeds are explicit and distinct"
    for e in entries:
        assert e.dataset_id.startswith("derived_")
        assert e.pinned_checksum is not None
        meta = read_json(REPO.metadata / f"{e.dataset_id}.json")
        assert meta["graph_checksum_fnv1a64"] == e.pinned_checksum
        assert meta["weights"]["synthesized"] is True
        assert meta["derived_from"] == e.base
        assert meta["transform"] == e.transform
        assert meta["seed"] == e.seed
        assert meta["max_weight"] < 2**31
        assert meta["min_weight"] >= 1
        assert meta["max_path_cost_bound"] < 2**63
        base_meta = read_json(REPO.metadata / f"{e.base}.json")
        for key in ("node_count", "arc_count", "edge_count", "directed", "has_coordinates"):
            assert meta[key] == base_meta[key], key

        base_first30 = read_manifest(REPO.manifests / f"{e.base}_queries_first30.yaml")
        derived_first30 = read_manifest(REPO.manifests / f"{e.dataset_id}_queries_first30.yaml")
        assert derived_first30.pairs == base_first30.pairs
        assert derived_first30.graph_checksum == e.pinned_checksum
        assert derived_first30.dataset_id == e.dataset_id

        graph = REPO.processed / f"{e.dataset_id}.lpqgraph"
        if graph.exists():  # processed graphs are git-ignored
            assert read_binary_graph_header(graph).checksum == e.pinned_checksum
