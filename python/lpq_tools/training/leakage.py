"""Training/evaluation split provenance (LPQ Atlas spec D-10, section 31).

The project once trained a model on the very queries it was then benchmarked
on (docs/experiments/static-scorer-training.md). This module makes that
mistake mechanically detectable: it compares a training query manifest with an
evaluation manifest and reports whether they are disjoint.

Two overlaps are distinguished because they mean different things:

* **pair overlap** -- the same (source, target) query appears in both sets. This
  is leakage in the strict sense and makes the split ``OVERLAPPING``.
* **endpoint overlap** -- a node is a source or target in both sets without the
  pair repeating. On a graph of hundreds of thousands of nodes with uniformly
  drawn endpoints a few coincidences are expected; it is reported, never hidden,
  but does not by itself fail the split.

A split between manifests of different graphs is ``INCOMPARABLE``: the node ids
do not refer to the same nodes.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from lpq_tools.data.queries import QueryManifest, read_manifest


@dataclass(frozen=True)
class SplitReport:
    status: str  # "DISJOINT" | "OVERLAPPING" | "INCOMPARABLE"
    training_manifest: str
    evaluation_manifest: str
    training_queries: int
    evaluation_queries: int
    shared_pairs: int
    shared_endpoints: int
    detail: str

    @property
    def disjoint(self) -> bool:
        return self.status == "DISJOINT"

    def as_dict(self) -> dict[str, object]:
        return dict(self.__dict__)


def compare_manifests(
    training: QueryManifest, evaluation: QueryManifest, train_name: str, eval_name: str
) -> SplitReport:
    same_graph = training.dataset_id == evaluation.dataset_id and (
        training.graph_checksum is None
        or evaluation.graph_checksum is None
        or training.graph_checksum == evaluation.graph_checksum
    )
    if not same_graph:
        return SplitReport(
            "INCOMPARABLE",
            train_name,
            eval_name,
            len(training.pairs),
            len(evaluation.pairs),
            0,
            0,
            "the manifests describe different graphs",
        )
    train_pairs = {(p.source, p.target) for p in training.pairs}
    eval_pairs = {(p.source, p.target) for p in evaluation.pairs}
    shared = train_pairs & eval_pairs
    train_nodes = {n for p in train_pairs for n in p}
    eval_nodes = {n for p in eval_pairs for n in p}
    shared_nodes = train_nodes & eval_nodes
    status = "OVERLAPPING" if shared else "DISJOINT"
    detail = (
        f"{len(shared)} query pair(s) appear in both sets"
        if shared
        else "no query pair appears in both sets"
    )
    return SplitReport(
        status,
        train_name,
        eval_name,
        len(training.pairs),
        len(evaluation.pairs),
        len(shared),
        len(shared_nodes),
        detail,
    )


def check_split(training_path: Path, evaluation_path: Path) -> SplitReport:
    return compare_manifests(
        read_manifest(training_path),
        read_manifest(evaluation_path),
        training_path.name,
        evaluation_path.name,
    )
