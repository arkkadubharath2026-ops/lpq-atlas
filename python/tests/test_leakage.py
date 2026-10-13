"""The automated training/evaluation leakage guard (spec section 31)."""

from __future__ import annotations

from pathlib import Path

import pytest
from lpq_tools.data.queries import QueryManifest, QueryPair
from lpq_tools.training.leakage import check_split, compare_manifests

_MANIFESTS = Path(__file__).resolve().parents[2] / "experiments" / "manifests"


def _m(dataset: str, pairs: list[tuple[int, int]], checksum: int | None = 7) -> QueryManifest:
    return QueryManifest(
        dataset_id=dataset,
        node_count=100,
        seed=1,
        pairs=tuple(QueryPair(s, t) for s, t in pairs),
        graph_checksum=checksum,
    )


def test_disjoint_sets_are_reported_disjoint() -> None:
    r = compare_manifests(_m("g", [(1, 2), (3, 4)]), _m("g", [(5, 6)]), "t", "e")
    assert r.status == "DISJOINT"
    assert r.shared_pairs == 0


def test_a_repeated_pair_is_leakage() -> None:
    r = compare_manifests(_m("g", [(1, 2), (3, 4)]), _m("g", [(3, 4), (9, 8)]), "t", "e")
    assert r.status == "OVERLAPPING"
    assert r.shared_pairs == 1


def test_shared_endpoints_are_reported_but_not_leakage() -> None:
    r = compare_manifests(_m("g", [(1, 2)]), _m("g", [(1, 5)]), "t", "e")
    assert r.status == "DISJOINT"
    assert r.shared_endpoints == 1


def test_different_graphs_are_incomparable() -> None:
    r = compare_manifests(_m("g", [(1, 2)]), _m("h", [(1, 2)]), "t", "e")
    assert r.status == "INCOMPARABLE"


@pytest.mark.parametrize("evaluation", ["dimacs_ny_queries_100.yaml", "dimacs_ny_queries.yaml"])
def test_committed_ny_training_set_is_disjoint_from_evaluation(evaluation: str) -> None:
    training = _MANIFESTS / "dimacs_ny_training_queries.yaml"
    target = _MANIFESTS / evaluation
    if not (training.exists() and target.exists()):
        pytest.skip("manifests not present")
    assert check_split(training, target).disjoint


# -----------------------------------------------------------------------------
# Source-level separation (D-21)
# -----------------------------------------------------------------------------
#
# lpq_collect labels from a full single-source search per source, so a training query that shares a
# SOURCE with an evaluation query reuses that search even when the pair differs. The corrected
# protocol (C) therefore requires source-level separation. It is enforced for training manifests that
# DECLARE it (selection "..._source_disjoint"); every other manifest keeps the legacy pair-level
# verdict, with the shared sources reported rather than hidden.


def _declared(dataset: str, pairs: list[tuple[int, int]], selection: str, checksum: int = 7):
    return QueryManifest(
        dataset_id=dataset,
        node_count=100,
        seed=1,
        pairs=tuple(QueryPair(s, t) for s, t in pairs),
        graph_checksum=checksum,
        selection=selection,
    )


def test_a_shared_source_is_reported_and_does_not_fail_the_legacy_pair_level_check() -> None:
    r = compare_manifests(_m("g", [(1, 2)]), _m("g", [(1, 5)]), "t", "e")
    assert (r.status, r.level, r.shared_pairs, r.shared_sources) == ("DISJOINT", "pair", 0, 1)


def test_the_source_level_check_fails_on_a_shared_source() -> None:
    r = compare_manifests(_m("g", [(1, 2)]), _m("g", [(1, 5)]), "t", "e", level="source")
    assert (r.status, r.level, r.shared_sources) == ("OVERLAPPING", "source", 1)
    assert "source" in r.detail


def test_a_shared_target_alone_is_not_a_source_overlap() -> None:
    r = compare_manifests(_m("g", [(1, 9)]), _m("g", [(2, 9)]), "t", "e", level="source")
    assert (r.status, r.shared_sources) == ("DISJOINT", 0)


def test_an_unknown_level_is_rejected() -> None:
    with pytest.raises(ValueError, match="level"):
        compare_manifests(_m("g", [(1, 2)]), _m("g", [(3, 4)]), "t", "e", level="endpoint")


def test_the_report_dictionary_keeps_every_legacy_key_and_adds_the_new_ones() -> None:
    d = compare_manifests(_m("g", [(1, 2)]), _m("g", [(3, 4)]), "t", "e").as_dict()
    legacy = {
        "status", "training_manifest", "evaluation_manifest", "training_queries",
        "evaluation_queries", "shared_pairs", "shared_endpoints", "detail",
    }
    assert legacy <= set(d)
    assert {"shared_sources", "level"} <= set(d)


def test_a_manifest_that_declares_source_disjointness_is_held_to_it(tmp_path: Path) -> None:
    from lpq_tools.data.queries import SELECTION_SOURCE_DISJOINT, write_manifest

    write_manifest(tmp_path / "t.yaml", _declared("g", [(1, 2)], SELECTION_SOURCE_DISJOINT))
    write_manifest(tmp_path / "e.yaml", _m("g", [(1, 5)]))
    r = check_split(tmp_path / "t.yaml", tmp_path / "e.yaml")
    assert (r.status, r.level, r.shared_sources) == ("OVERLAPPING", "source", 1)


def test_a_legacy_manifest_keeps_the_pair_level_verdict_but_shows_the_overlap(tmp_path: Path) -> None:
    from lpq_tools.data.queries import write_manifest

    write_manifest(tmp_path / "t.yaml", _m("g", [(1, 2)]))
    write_manifest(tmp_path / "e.yaml", _m("g", [(1, 5)]))
    r = check_split(tmp_path / "t.yaml", tmp_path / "e.yaml")
    assert (r.status, r.level, r.shared_sources) == ("DISJOINT", "pair", 1)


def test_an_explicit_level_overrides_the_manifests_declaration(tmp_path: Path) -> None:
    from lpq_tools.data.queries import write_manifest

    write_manifest(tmp_path / "t.yaml", _m("g", [(1, 2)]))
    write_manifest(tmp_path / "e.yaml", _m("g", [(1, 5)]))
    assert check_split(tmp_path / "t.yaml", tmp_path / "e.yaml", level="source").status == "OVERLAPPING"


@pytest.mark.parametrize(
    "evaluation",
    ["snap_facebook_queries.yaml", "snap_facebook_queries_first100.yaml", "snap_facebook_queries_first30.yaml"],
)
def test_the_committed_facebook_protocol_c_set_passes_the_source_level_check(evaluation: str) -> None:
    training = _MANIFESTS / "snap_facebook_training_queries_source_disjoint.yaml"
    r = check_split(training, _MANIFESTS / evaluation)
    assert (r.status, r.level, r.shared_sources) == ("DISJOINT", "source", 0)


def test_the_committed_pair_disjoint_facebook_set_stays_legacy_and_shows_its_45_shared_sources() -> None:
    r = check_split(
        _MANIFESTS / "snap_facebook_training_queries.yaml", _MANIFESTS / "snap_facebook_queries.yaml"
    )
    assert (r.status, r.level, r.shared_sources) == ("DISJOINT", "pair", 45)
