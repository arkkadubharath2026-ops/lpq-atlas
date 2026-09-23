"""End-to-end tests for the dataset pipeline.

These run the REAL ``lpq_convert`` executable against small graphs written by
the test. A mocked converter would test only that the pipeline can call a
subprocess; what actually needs proving is that the Python and C++ halves agree
about the binary header, the checksum and the metadata, since that agreement is
the whole reason the pipeline is split across two languages.

The tests skip rather than fail when the executable has not been built, because
a Python-only checkout is a legitimate state -- but the skip message says what
to build, so a skipped run is actionable rather than silent.
"""

from __future__ import annotations

import gzip
import json
import struct

import pytest

from lpq_tools.data import acquire
from lpq_tools.data.binary_header import (
    GRAPH_FORMAT_VERSION,
    BinaryGraphError,
    read_binary_graph_header,
)
from lpq_tools.data.pipeline import PipelineError, prepare_all, prepare_dataset, summarise
from lpq_tools.data.queries import read_manifest
from lpq_tools.data.sources import parse_dataset_config
from lpq_tools.utilities.jsonio import read_json
from lpq_tools.utilities.paths import RepoPaths


@pytest.fixture(scope="session")
def converter():
    try:
        return RepoPaths.discover().converter()
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


@pytest.fixture
def workspace(tmp_path, monkeypatch):
    """A self-contained repository-shaped tree, with downloads served locally."""
    for marker in ("PRD.md", "ESD.md", "CMakePresets.json"):
        (tmp_path / marker).write_text("test fixture\n", encoding="utf-8")

    served: dict[str, bytes] = {}

    def fake_download(url, destination, *, progress=None, timeout=60.0):
        if url not in served:
            raise acquire.AcquisitionError(f"{url}: HTTP 404 Not Found")
        destination.parent.mkdir(parents=True, exist_ok=True)
        acquire._make_writable(destination)
        destination.write_bytes(served[url])

    monkeypatch.setattr(acquire, "download_file", fake_download)

    # The fixture root has no build tree, so point the converter lookup at the
    # real executable. Exercising the real binary is the entire point of these
    # tests; only the directory layout is synthetic.
    try:
        monkeypatch.setenv("LPQ_CONVERT", str(RepoPaths.discover().converter()))
    except FileNotFoundError as exc:
        pytest.skip(str(exc))

    paths = RepoPaths(tmp_path)
    paths.ensure_directories()
    paths.dataset_configs.mkdir(parents=True, exist_ok=True)
    return paths, served


def gzipped(text: str) -> bytes:
    import io

    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", mtime=0) as handle:
        handle.write(text.encode("utf-8"))
    return buffer.getvalue()


def snap_config(**overrides) -> dict:
    base = {
        "dataset_id": "toy_snap",
        "source_format": "snap",
        "files": [{"url": "https://x.invalid/toy.txt.gz", "role": "graph"}],
        "directedness": "undirected",
        "weights": {
            "distribution": "uniform",
            "minimum": 1,
            "maximum": 1000,
            "seed": 4242,
        },
        "query_count": 32,
        "query_seed": 7,
    }
    base.update(overrides)
    return base


# A small ring, written in the SNAP style: undirected, unweighted, sparse ids.
TOY_SNAP = "# toy ring\n" + "".join(f"{10 * i} {10 * ((i + 1) % 8)}\n" for i in range(8))

# A DIMACS graph, with both directions given explicitly, plus coordinates.
TOY_DIMACS = "c toy\np sp 4 6\n" + "".join(
    f"a {u} {v} {w}\n"
    for u, v, w in [(1, 2, 10), (2, 1, 10), (2, 3, 20), (3, 2, 20), (3, 4, 30), (4, 3, 30)]
)
TOY_DIMACS_CO = "c toy coordinates\np aux sp co 4\n" + "".join(
    f"v {i} {1000 * i} {2000 * i}\n" for i in range(1, 5)
)


# -----------------------------------------------------------------------------
# The whole pipeline
# -----------------------------------------------------------------------------


def test_prepares_a_snap_dataset_end_to_end(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    result = prepare_dataset(config, paths)

    assert result.converted
    assert result.graph_path.exists()
    assert result.metadata_path.exists()
    assert result.manifest_path.exists()

    # A ring of 8 nodes, stored undirected: 8 edges, 16 arcs.
    assert result.header.node_count == 8
    assert result.header.arc_count == 16
    assert result.header.edge_count == 8
    assert not result.header.directed
    assert result.header.has_mapping  # SNAP ids were sparse and must be recoverable
    assert not result.header.has_coordinates
    assert result.header.weight_type == "uint32"


def test_raw_files_are_never_modified(workspace, converter):
    paths, served = workspace
    payload = gzipped(TOY_SNAP)
    served["https://x.invalid/toy.txt.gz"] = payload
    config = parse_dataset_config(snap_config())

    prepare_dataset(config, paths)
    prepare_dataset(config, paths, force=True)

    assert (paths.raw / "toy.txt.gz").read_bytes() == payload
    # The expansion goes to staging, not back into raw.
    assert (paths.staging / "toy.txt").exists()
    assert not (paths.raw / "toy.txt").exists()


def test_metadata_records_that_the_weights_are_synthetic(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    result = prepare_dataset(config, paths)
    metadata = read_json(result.metadata_path)

    # Without this, a results table would attribute a synthetic weight
    # distribution to the SNAP dataset.
    assert metadata["weights"]["synthesized"] is True
    assert metadata["weights"]["distribution"] == "uniform"
    assert metadata["weights"]["seed"] == 4242
    assert metadata["node_count"] == 8
    assert metadata["edge_count"] == 8
    assert metadata["min_weight"] >= 1
    assert metadata["max_weight"] <= 1000


def test_the_metadata_checksum_matches_the_graph_file(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    result = prepare_dataset(config, paths)
    metadata = read_json(result.metadata_path)
    # The agreement the split-language design depends on: Python reads the same
    # checksum out of the header that C++ wrote into the metadata.
    assert metadata["graph_checksum_fnv1a64"] == result.header.checksum


def test_the_query_manifest_is_bound_to_the_graph_it_was_generated_for(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    result = prepare_dataset(config, paths)
    manifest = read_manifest(result.manifest_path)

    assert manifest.dataset_id == "toy_snap"
    assert manifest.node_count == result.header.node_count
    assert manifest.graph_checksum == result.header.checksum
    assert len(manifest.pairs) == 32
    for pair in manifest.pairs:
        assert 0 <= pair.source < result.header.node_count
        assert 0 <= pair.target < result.header.node_count
        assert pair.source != pair.target


def test_a_second_run_does_no_work(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    first = prepare_dataset(config, paths)
    second = prepare_dataset(config, paths)

    assert first.converted
    assert not second.converted
    assert second.header.checksum == first.header.checksum


def test_changing_the_weight_seed_forces_a_reconversion(workspace, converter):
    # The case a presence check alone would get wrong: same inputs, same output
    # path, different graph.
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)

    first = prepare_dataset(parse_dataset_config(snap_config()), paths)
    changed = snap_config()
    changed["weights"] = dict(changed["weights"], seed=999)
    second = prepare_dataset(parse_dataset_config(changed), paths)

    assert second.converted
    assert second.header.checksum != first.header.checksum


def test_changing_nothing_keeps_the_checksum_stable(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())

    first = prepare_dataset(config, paths)
    second = prepare_dataset(config, paths, force=True)
    assert second.header.checksum == first.header.checksum


# -----------------------------------------------------------------------------
# DIMACS, with coordinates
# -----------------------------------------------------------------------------


def dimacs_config() -> dict:
    return {
        "dataset_id": "toy_dimacs",
        "source_format": "dimacs",
        "files": [
            {"url": "https://x.invalid/toy.gr.gz", "role": "graph"},
            {"url": "https://x.invalid/toy.co.gz", "role": "coordinates"},
        ],
        "directedness": "directed",
        "query_count": 16,
        "query_seed": 3,
    }


def test_prepares_a_dimacs_dataset_with_coordinates(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.gr.gz"] = gzipped(TOY_DIMACS)
    served["https://x.invalid/toy.co.gz"] = gzipped(TOY_DIMACS_CO)

    result = prepare_dataset(parse_dataset_config(dimacs_config()), paths)

    assert result.header.node_count == 4
    assert result.header.arc_count == 6
    # DIMACS lists both directions, so the arcs are already paired and the
    # graph is stored directed.
    assert result.header.directed
    assert result.header.edge_count == 6
    # Coordinates are what make A* a meaningful baseline rather than Dijkstra
    # with extra steps.
    assert result.header.has_coordinates


def test_dimacs_weights_are_recorded_as_the_datasets_own(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.gr.gz"] = gzipped(TOY_DIMACS)
    served["https://x.invalid/toy.co.gz"] = gzipped(TOY_DIMACS_CO)

    result = prepare_dataset(parse_dataset_config(dimacs_config()), paths)
    metadata = read_json(result.metadata_path)

    assert metadata["weights"]["synthesized"] is False
    assert metadata["min_weight"] == 10
    assert metadata["max_weight"] == 30
    assert metadata["has_coordinates"] is True


# -----------------------------------------------------------------------------
# Failure handling
# -----------------------------------------------------------------------------


def test_a_truncated_dimacs_file_is_caught_before_conversion(workspace, converter):
    # The download succeeded and the file parses; only the header disagreement
    # reveals that it is not the whole graph.
    paths, served = workspace
    truncated = "c toy\np sp 4 6\na 1 2 10\na 2 1 10\n"
    served["https://x.invalid/toy.gr.gz"] = gzipped(truncated)
    served["https://x.invalid/toy.co.gz"] = gzipped(TOY_DIMACS_CO)

    with pytest.raises(PipelineError, match="declares 6 arcs"):
        prepare_dataset(parse_dataset_config(dimacs_config()), paths)


def test_a_partial_coordinate_file_is_caught(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.gr.gz"] = gzipped(TOY_DIMACS)
    served["https://x.invalid/toy.co.gz"] = gzipped("v 1 10 20\nv 2 30 40\n")

    with pytest.raises(PipelineError, match="coordinates"):
        prepare_dataset(parse_dataset_config(dimacs_config()), paths)


def test_skip_download_fails_loudly_when_the_raw_file_is_absent(workspace, converter):
    paths, _ = workspace
    with pytest.raises(PipelineError, match="skip-download"):
        prepare_dataset(parse_dataset_config(snap_config()), paths, skip_download=True)


def test_a_tampered_metadata_record_is_detected(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    config = parse_dataset_config(snap_config())
    result = prepare_dataset(config, paths)

    metadata = read_json(result.metadata_path)
    metadata["graph_checksum_fnv1a64"] = 1
    result.metadata_path.write_text(json.dumps(metadata), encoding="utf-8")

    # Trusting the record here would attribute one graph's results to another.
    with pytest.raises(PipelineError, match="checksum"):
        prepare_dataset(config, paths)


def test_prepare_all_collects_failures_without_abandoning_the_rest(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    # The DIMACS URLs are deliberately not served.
    configs = [parse_dataset_config(snap_config()), parse_dataset_config(dimacs_config())]

    results, failures = prepare_all(configs, paths)

    assert [r.dataset_id for r in results] == ["toy_snap"]
    assert [name for name, _ in failures] == ["toy_dimacs"]


def test_the_summary_index_describes_every_prepared_dataset(workspace, converter):
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(TOY_SNAP)
    result = prepare_dataset(parse_dataset_config(snap_config()), paths)

    index = summarise([result])
    entry = index["datasets"][0]
    assert entry["dataset_id"] == "toy_snap"
    assert entry["node_count"] == 8
    assert entry["graph_checksum_fnv1a64"] == result.header.checksum
    # The file had no published checksum, and the index must say so rather than
    # implying it was verified.
    assert entry["files_accepted_on_trust"] == ["toy.txt.gz"]


# -----------------------------------------------------------------------------
# Header parsing
# -----------------------------------------------------------------------------


def test_a_non_graph_file_is_rejected(tmp_path):
    path = tmp_path / "not-a-graph.bin"
    path.write_bytes(b"\x00" * 64)
    with pytest.raises(BinaryGraphError, match="magic"):
        read_binary_graph_header(path)


def test_a_short_file_is_rejected(tmp_path):
    path = tmp_path / "short.bin"
    path.write_bytes(b"\x00" * 8)
    with pytest.raises(BinaryGraphError, match="too short"):
        read_binary_graph_header(path)


def test_a_future_format_version_is_rejected_rather_than_guessed(tmp_path):
    # A header whose fields have moved would otherwise be read as plausible
    # nonsense.
    path = tmp_path / "future.bin"
    path.write_bytes(
        struct.pack(
            "<QIIIQQIIQ",
            0x485041524751504C,
            GRAPH_FORMAT_VERSION + 1,
            1,
            0,
            10,
            20,
            0,
            0,
            123,
        )
    )
    with pytest.raises(BinaryGraphError, match="version"):
        read_binary_graph_header(path)


def test_an_unknown_weight_tag_is_rejected(tmp_path):
    path = tmp_path / "weird.bin"
    path.write_bytes(
        struct.pack(
            "<QIIIQQIIQ", 0x485041524751504C, GRAPH_FORMAT_VERSION, 99, 0, 10, 20, 0, 0, 123
        )
    )
    with pytest.raises(BinaryGraphError, match="weight type"):
        read_binary_graph_header(path)


# -----------------------------------------------------------------------------
# The configs in the repository work with the real converter
# -----------------------------------------------------------------------------


def test_checked_in_configs_produce_valid_converter_commands(workspace, converter):
    """Every checked-in config must yield flags lpq_convert actually accepts.

    Catches the class of bug where a config names a policy that the Python
    parser allows and the C++ tool rejects -- which would otherwise only surface
    after a multi-gigabyte download.
    """
    from lpq_tools.data.pipeline import _converter_command

    real_paths = RepoPaths.discover()
    from lpq_tools.data.sources import load_all_dataset_configs

    paths, _ = workspace
    for config in load_all_dataset_configs(real_paths.dataset_configs):
        command = _converter_command(
            config,
            converter,
            paths.staging / "input",
            (paths.staging / "coords") if config.coordinate_file else None,
            paths.processed / "out.lpqgraph",
            paths.metadata / "out.json",
        )
        assert "--format" in command
        assert config.source_format in command
        if config.weights is not None:
            assert "--weights" in command
            assert config.weights.distribution in command


# -----------------------------------------------------------------------------
# Edge-listing convention (regression)
# -----------------------------------------------------------------------------


def test_a_doubly_listed_snap_file_is_not_paired_again(workspace, converter):
    # A ring of 8 nodes, with both directions written out: 16 records for 8
    # edges. Declaring edges_listed: both must yield 8 edges, not 16.
    body = "".join(
        f"{10 * i} {10 * ((i + 1) % 8)}\n{10 * ((i + 1) % 8)} {10 * i}\n" for i in range(8)
    )
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(body)

    result = prepare_dataset(
        parse_dataset_config(snap_config(edges_listed="both")), paths
    )
    assert result.header.node_count == 8
    assert result.header.arc_count == 16
    assert result.header.edge_count == 8
    assert not result.header.directed


def test_a_mis_declared_edge_listing_is_rejected(workspace, converter):
    # The check that would have caught the roadNet conversions: the file lists
    # both directions but the config claims each edge appears once.
    body = "".join(f"{i} {i + 1}\n{i + 1} {i}\n" for i in range(2000))
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(body)

    with pytest.raises(PipelineError, match="edges_listed"):
        prepare_dataset(parse_dataset_config(snap_config(edges_listed="once")), paths)


def test_the_declared_edge_listing_reaches_the_metadata(workspace, converter):
    body = "".join(f"{i} {i + 1}\n{i + 1} {i}\n" for i in range(2000))
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(body)

    result = prepare_dataset(parse_dataset_config(snap_config(edges_listed="both")), paths)
    assert read_json(result.metadata_path)["normalization"]["edges_listed"] == "both"


def test_changing_the_edge_listing_forces_a_reconversion(workspace, converter):
    # Deliberately small: below the detector's minimum sample the convention is
    # reported as unknown, so the cross-check stands aside and the declaration
    # is used as given. That is what lets this test vary the declaration alone,
    # and it is also the real behaviour on a graph too small to infer from.
    body = "".join(f"{i} {i + 1}\n" for i in range(20))
    paths, served = workspace
    served["https://x.invalid/toy.txt.gz"] = gzipped(body)

    first = prepare_dataset(parse_dataset_config(snap_config(edges_listed="once")), paths)
    second = prepare_dataset(parse_dataset_config(snap_config(edges_listed="both")), paths)
    assert second.converted
    assert second.header.arc_count != first.header.arc_count
