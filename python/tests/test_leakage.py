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
