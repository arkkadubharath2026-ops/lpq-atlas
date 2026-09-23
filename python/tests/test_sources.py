"""Tests for dataset config parsing.

These configs decide what gets downloaded and how it is normalized, so the
parser's job is to reject anything ambiguous rather than to fill in a default.
A config that parses but means something other than what it says would produce
a perfectly clean run of the wrong experiment.
"""

from __future__ import annotations

import pytest
import yaml

from lpq_tools.data.sources import (
    DatasetConfigError,
    load_all_dataset_configs,
    load_dataset_config,
    parse_dataset_config,
)
from lpq_tools.utilities.paths import RepoPaths


def minimal(**overrides) -> dict:
    base = {
        "dataset_id": "toy",
        "source_format": "snap",
        "files": [{"url": "https://example.invalid/toy.txt.gz", "role": "graph"}],
    }
    base.update(overrides)
    return base


# -----------------------------------------------------------------------------
# Happy path
# -----------------------------------------------------------------------------


def test_parses_a_minimal_config():
    config = parse_dataset_config(minimal())
    assert config.dataset_id == "toy"
    assert config.source_format == "snap"
    assert config.graph_file.filename == "toy.txt.gz"
    assert config.graph_file.is_compressed
    assert config.graph_file.decompressed_name == "toy.txt"
    assert config.coordinate_file is None
    assert config.weights is None


def test_defaults_match_the_converters_own_defaults():
    config = parse_dataset_config(minimal())
    assert config.directedness == "undirected"
    assert config.self_loops == "drop"
    assert config.parallel_edges == "keep"
    assert config.default_weight == 1


def test_an_explicit_filename_overrides_the_url_tail():
    config = parse_dataset_config(
        minimal(files=[{"url": "https://example.invalid/download?id=7", "filename": "toy.txt"}])
    )
    assert config.graph_file.filename == "toy.txt"


def test_weight_synthesis_is_translated_to_converter_flags():
    config = parse_dataset_config(
        minimal(
            weights={
                "distribution": "lognormal",
                "minimum": 1,
                "maximum": 1000,
                "sigma": 1.5,
                "seed": 77,
            }
        )
    )
    args = config.weights.to_arguments()
    assert "--weights" in args and "lognormal" in args
    assert "--weight-seed" in args and "77" in args
    assert "--weight-sigma" in args
    # Parameters that belong to other families must not be passed along.
    assert "--weight-alpha" not in args
    assert "--narrow-fraction" not in args


def test_unpinned_files_are_reported():
    config = parse_dataset_config(minimal())
    assert len(config.unpinned_files) == 1

    pinned = parse_dataset_config(
        minimal(files=[{"url": "https://example.invalid/toy.txt", "sha256": "ab" * 32}])
    )
    assert pinned.unpinned_files == ()


# -----------------------------------------------------------------------------
# Rejections
# -----------------------------------------------------------------------------


def test_rejects_an_unknown_source_format():
    with pytest.raises(DatasetConfigError, match="source_format"):
        parse_dataset_config(minimal(source_format="graphml"))


def test_rejects_an_unknown_weight_distribution():
    with pytest.raises(DatasetConfigError, match="distribution"):
        parse_dataset_config(
            minimal(weights={"distribution": "gaussian", "minimum": 1, "maximum": 2, "seed": 1})
        )


def test_rejects_weight_synthesis_without_a_seed():
    # An unseeded synthesis is not reproducible, and a default seed would hide
    # that the config never said which graph it meant.
    with pytest.raises(DatasetConfigError, match="seed"):
        parse_dataset_config(
            minimal(weights={"distribution": "uniform", "minimum": 1, "maximum": 2})
        )


def test_rejects_inverted_weight_bounds():
    with pytest.raises(DatasetConfigError):
        parse_dataset_config(
            minimal(weights={"distribution": "uniform", "minimum": 9, "maximum": 2, "seed": 1})
        )


def test_rejects_a_malformed_sha256():
    with pytest.raises(DatasetConfigError, match="64 hex"):
        parse_dataset_config(
            minimal(files=[{"url": "https://example.invalid/toy.txt", "sha256": "deadbeef"}])
        )


def test_rejects_two_graph_files():
    with pytest.raises(DatasetConfigError, match="exactly one"):
        parse_dataset_config(
            minimal(
                files=[
                    {"url": "https://example.invalid/a.txt", "role": "graph"},
                    {"url": "https://example.invalid/b.txt", "role": "graph"},
                ]
            )
        )


def test_rejects_no_graph_file():
    with pytest.raises(DatasetConfigError, match="exactly one"):
        parse_dataset_config(
            minimal(files=[{"url": "https://example.invalid/a.co", "role": "coordinates"}])
        )


def test_rejects_a_duplicate_raw_filename():
    # Two entries writing the same raw file would race, and the second would
    # appear to succeed against the first one's contents.
    with pytest.raises(DatasetConfigError, match="duplicate"):
        parse_dataset_config(
            minimal(
                files=[
                    {"url": "https://a.invalid/toy.txt", "role": "graph"},
                    {"url": "https://b.invalid/toy.txt", "role": "coordinates"},
                ]
            )
        )


def test_rejects_an_unknown_file_role():
    with pytest.raises(DatasetConfigError, match="role"):
        parse_dataset_config(
            minimal(files=[{"url": "https://example.invalid/a.txt", "role": "weights"}])
        )


def test_rejects_coordinates_on_a_non_dimacs_dataset():
    # SNAP edge lists have no coordinate file, so declaring one means the config
    # is describing a different dataset from the one it names.
    with pytest.raises(DatasetConfigError, match="DIMACS"):
        parse_dataset_config(
            minimal(
                source_format="snap",
                files=[
                    {"url": "https://example.invalid/a.txt", "role": "graph"},
                    {"url": "https://example.invalid/a.co", "role": "coordinates"},
                ],
            )
        )


def test_rejects_an_unknown_parallel_edge_policy():
    with pytest.raises(DatasetConfigError, match="parallel_edges"):
        parse_dataset_config(minimal(parallel_edges="collapse"))


def test_rejects_a_non_positive_query_count():
    with pytest.raises(DatasetConfigError, match="query_count"):
        parse_dataset_config(minimal(query_count=0))


def test_rejects_duplicate_dataset_ids_across_files(tmp_path):
    for name in ("a.yaml", "b.yaml"):
        (tmp_path / name).write_text(yaml.safe_dump(minimal()), encoding="utf-8")
    with pytest.raises(DatasetConfigError, match="two configs"):
        load_all_dataset_configs(tmp_path)


# -----------------------------------------------------------------------------
# The configs actually checked into the repository
# -----------------------------------------------------------------------------


def test_every_checked_in_config_parses():
    paths = RepoPaths.discover()
    configs = load_all_dataset_configs(paths.dataset_configs)
    assert configs, "no dataset configs found"
    for config in configs:
        assert config.dataset_id
        assert config.graph_file.url.startswith("http")


def test_dimacs_configs_declare_coordinates_and_real_weights():
    paths = RepoPaths.discover()
    for config in load_all_dataset_configs(paths.dataset_configs):
        if not config.dataset_id.startswith("dimacs_"):
            continue
        assert config.source_format == "dimacs"
        assert config.coordinate_file is not None, config.dataset_id
        # Synthesising over a graph that carries real travel costs would discard
        # the only real metric in the collection.
        assert config.weights is None, config.dataset_id
        # DIMACS files list both directions explicitly.
        assert config.directedness == "directed", config.dataset_id


def test_snap_configs_declare_synthetic_weights():
    paths = RepoPaths.discover()
    for config in load_all_dataset_configs(paths.dataset_configs):
        if not config.dataset_id.startswith("snap_"):
            continue
        # SNAP publishes these unweighted, so a weight has to come from
        # somewhere and the config must say where.
        assert config.weights is not None, config.dataset_id
        assert config.weights.seed != 0, config.dataset_id


def test_each_config_file_declares_the_dataset_id_its_filename_implies():
    paths = RepoPaths.discover()
    for path in sorted(paths.dataset_configs.glob("*.yaml")):
        config = load_dataset_config(path)
        assert config.dataset_id == path.stem, path.name
