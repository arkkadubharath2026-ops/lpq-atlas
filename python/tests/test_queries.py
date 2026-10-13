"""Tests for query manifest generation.

The properties that matter are determinism and unbiasedness, because a manifest
is the thing that makes every queue's measurements comparable. If it drifts, the
paired comparison silently becomes unpaired; if it is biased towards particular
nodes, the benchmark measures a corner of the graph.
"""

from __future__ import annotations

import pytest

from lpq_tools.data.queries import (
    QueryPair,
    SplitMix64,
    build_manifest,
    derive_seed,
    generate_query_pairs,
    read_manifest,
    write_manifest,
)


# -----------------------------------------------------------------------------
# The generator itself
# -----------------------------------------------------------------------------


def test_splitmix64_is_reproducible_from_a_seed():
    first = [SplitMix64(12345).next_u64() for _ in range(10)]
    second = [SplitMix64(12345).next_u64() for _ in range(10)]
    assert first == second


def test_splitmix64_matches_its_published_stream():
    # The reference stream for seed 0, so a future refactor cannot quietly
    # change the algorithm while keeping it self-consistent. These values are
    # SplitMix64's specified output and are checked in as the contract.
    rng = SplitMix64(0)
    assert rng.next_u64() == 0xE220A8397B1DCDAF
    assert rng.next_u64() == 0x6E789E6AA1B965F4
    assert rng.next_u64() == 0x06C45D188009454F


def test_next_below_stays_in_range():
    rng = SplitMix64(7)
    for _ in range(2000):
        assert 0 <= rng.next_below(10) < 10


def test_next_below_rejects_a_non_positive_bound():
    with pytest.raises(ValueError):
        SplitMix64(1).next_below(0)


def test_next_below_is_close_to_uniform():
    # A modulo fold would skew the low values; this catches a gross skew without
    # pretending to be a statistical test.
    rng = SplitMix64(99)
    counts = [0] * 8
    draws = 80_000
    for _ in range(draws):
        counts[rng.next_below(8)] += 1
    expected = draws / 8
    for count in counts:
        assert abs(count - expected) < 0.05 * expected


def test_derived_seeds_differ_by_label():
    assert derive_seed(42, "queries") != derive_seed(42, "perturbations")
    assert derive_seed(42, "queries") == derive_seed(42, "queries")
    assert derive_seed(1, "queries") != derive_seed(2, "queries")


# -----------------------------------------------------------------------------
# Pair generation
# -----------------------------------------------------------------------------


def test_pairs_are_deterministic():
    a = generate_query_pairs(1000, 200, seed=42)
    b = generate_query_pairs(1000, 200, seed=42)
    assert a == b


def test_a_different_seed_gives_different_pairs():
    a = generate_query_pairs(1000, 200, seed=42)
    b = generate_query_pairs(1000, 200, seed=43)
    assert a != b


def test_endpoints_are_always_distinct():
    # A query from a node to itself costs zero without touching the queue, so it
    # would dilute the measurement with a non-query.
    for pair in generate_query_pairs(50, 5000, seed=3):
        assert pair.source != pair.target


def test_endpoints_are_in_range():
    node_count = 37
    for pair in generate_query_pairs(node_count, 4000, seed=11):
        assert 0 <= pair.source < node_count
        assert 0 <= pair.target < node_count


def test_the_smallest_possible_graph_works():
    pairs = generate_query_pairs(2, 20, seed=5)
    assert len(pairs) == 20
    for pair in pairs:
        assert {pair.source, pair.target} == {0, 1}


def test_a_one_node_graph_is_rejected():
    with pytest.raises(ValueError):
        generate_query_pairs(1, 10, seed=5)


def test_a_non_positive_query_count_is_rejected():
    with pytest.raises(ValueError):
        generate_query_pairs(100, 0, seed=5)


def test_targets_are_not_biased_away_from_low_ids():
    # The "skip over the source" remap is the step that could introduce a bias.
    # Every node should appear as a target at roughly the same rate.
    node_count = 16
    counts = [0] * node_count
    for pair in generate_query_pairs(node_count, 64_000, seed=17):
        counts[pair.target] += 1
    expected = 64_000 / node_count
    for count in counts:
        assert abs(count - expected) < 0.08 * expected


def test_a_longer_run_extends_a_shorter_one():
    # The stream must not depend on how many pairs were asked for, so a manifest
    # regenerated with a larger query count keeps the earlier queries and the
    # two runs stay comparable on their common prefix.
    short = generate_query_pairs(500, 100, seed=8)
    long = generate_query_pairs(500, 300, seed=8)
    assert long[:100] == short


# -----------------------------------------------------------------------------
# Manifest round trip
# -----------------------------------------------------------------------------


def test_manifest_round_trips_through_yaml(tmp_path):
    manifest = build_manifest(
        "toy", node_count=500, count=64, seed=42, graph_checksum=0xDEADBEEFCAFEF00D
    )
    path = tmp_path / "toy_queries.yaml"
    write_manifest(path, manifest)

    back = read_manifest(path)
    assert back.dataset_id == manifest.dataset_id
    assert back.node_count == manifest.node_count
    assert back.seed == manifest.seed
    assert back.graph_checksum == manifest.graph_checksum
    assert back.pairs == manifest.pairs


def test_manifest_records_that_it_did_not_filter_for_reachability(tmp_path):
    # Claiming pairs were filtered when they were not would misrepresent what a
    # "no path" result means downstream.
    manifest = build_manifest("toy", node_count=100, count=4, seed=1)
    assert manifest.to_dict()["reachability_filtered"] is False


def test_manifest_carries_the_graph_checksum(tmp_path):
    # Node ids are dense indices assigned during conversion, so a manifest only
    # means anything against the exact graph it was generated for.
    manifest = build_manifest("toy", node_count=100, count=4, seed=1, graph_checksum=1234)
    assert manifest.to_dict()["graph_checksum_fnv1a64"] == 1234


def test_manifest_file_is_regenerable_byte_for_byte(tmp_path):
    manifest = build_manifest("toy", node_count=900, count=50, seed=42, graph_checksum=7)
    first = tmp_path / "a.yaml"
    second = tmp_path / "b.yaml"
    write_manifest(first, manifest)
    write_manifest(second, build_manifest("toy", node_count=900, count=50, seed=42, graph_checksum=7))
    assert first.read_bytes() == second.read_bytes()


def test_query_pair_is_hashable_and_comparable():
    assert QueryPair(1, 2) == QueryPair(1, 2)
    assert len({QueryPair(1, 2), QueryPair(1, 2), QueryPair(2, 1)}) == 2


# -----------------------------------------------------------------------------
# Source exclusion (M5-P1: source-disjoint training manifests)
# -----------------------------------------------------------------------------
#
# lpq_collect labels from a FULL single-source search per query, so a training query that shares a
# SOURCE with a benchmark query reuses the same search, even when the (source, target) pairs differ.
# Excluding sources is the separation that matches how the labels are made.


def test_the_default_stream_is_unchanged_by_the_exclusion_option():
    # Pinned from the generator as it was before the option existed: every committed manifest was
    # produced this way, and a regeneration that moved a single pair would break their pairing.
    pairs = generate_query_pairs(1000, 200, seed=42)
    assert [(p.source, p.target) for p in pairs[:3]] == [(399, 93), (905, 407), (95, 367)]
    assert generate_query_pairs(1000, 200, seed=42, exclude_sources=()) == pairs
    assert generate_query_pairs(1000, 200, seed=42, exclude_sources=None) == pairs


def test_excluded_sources_never_appear_as_sources():
    excluded = set(range(0, 1000, 3))  # a third of the graph
    pairs = generate_query_pairs(1000, 5000, seed=9, exclude_sources=excluded)
    assert len(pairs) == 5000
    assert not {p.source for p in pairs} & excluded
    for p in pairs:
        assert p.source != p.target
        assert 0 <= p.target < 1000


def test_exclusion_is_deterministic_and_prefix_stable():
    excluded = {1, 2, 3, 400}
    a = generate_query_pairs(500, 100, seed=8, exclude_sources=excluded)
    assert a == generate_query_pairs(500, 100, seed=8, exclude_sources=excluded)
    assert generate_query_pairs(500, 300, seed=8, exclude_sources=excluded)[:100] == a
    assert a != generate_query_pairs(500, 100, seed=8)


def test_sources_remain_uniform_over_the_allowed_nodes():
    node_count, excluded = 16, {0, 5, 11}
    counts = {u: 0 for u in range(node_count) if u not in excluded}
    for p in generate_query_pairs(node_count, 52_000, seed=17, exclude_sources=excluded):
        counts[p.source] += 1
    expected = 52_000 / len(counts)
    for count in counts.values():
        assert abs(count - expected) < 0.08 * expected


def test_excluding_every_node_is_rejected():
    with pytest.raises(ValueError, match="no source"):
        generate_query_pairs(10, 5, seed=1, exclude_sources=set(range(10)))


def test_an_excluded_id_outside_the_graph_is_rejected():
    # An id from a manifest of a different graph is a mistake worth stopping on, not ignoring.
    with pytest.raises(ValueError, match="outside"):
        generate_query_pairs(10, 5, seed=1, exclude_sources={3, 10})


def test_a_source_excluded_manifest_says_so_and_stays_readable_by_the_engine(tmp_path):
    manifest = build_manifest(
        "toy", node_count=300, count=40, seed=917, graph_checksum=5, exclude_sources={1, 2, 3}
    )
    assert manifest.to_dict()["selection"] == "uniform_pairs_distinct_endpoints_source_disjoint"
    path = tmp_path / "m.yaml"
    write_manifest(path, manifest, notes=["excludes the sources of toy_queries.yaml"])
    text = path.read_text()
    assert "# excludes the sources of toy_queries.yaml" in text
    assert read_manifest(path).pairs == manifest.pairs
    assert read_manifest(path).selection == manifest.selection
    # The C++ reader rejects any top-level key it does not know; provenance therefore lives in
    # comments and in the free-form `selection` value, never in a new key.
    keys = {line.split(":")[0] for line in text.splitlines() if line and line[0] not in "#- "}
    assert keys <= {
        "dataset_id", "node_count", "seed", "graph_checksum_fnv1a64", "query_count",
        "generator", "selection", "reachability_filtered", "queries",
    }


def test_a_default_manifest_gains_no_new_lines(tmp_path):
    manifest = build_manifest("toy", node_count=900, count=50, seed=42, graph_checksum=7)
    assert manifest.to_dict()["selection"] == "uniform_pairs_distinct_endpoints"
    path = tmp_path / "a.yaml"
    write_manifest(path, manifest)
    comments = [line for line in path.read_text().splitlines() if line.startswith("#")]
    assert len(comments) == 5, "the original header, unchanged, and no notes"
