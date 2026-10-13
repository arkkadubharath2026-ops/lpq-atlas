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

**Source level (D-21).** The collector labels a training set from a full single-source
search per source, so a training query that shares a *source* with an evaluation query
reuses that search even when the pair differs. The corrected training protocol therefore
requires source-level separation, and ``level="source"`` makes a shared source fail the
split. The level is chosen automatically by :func:`check_split`: a training manifest whose
``selection`` declares ``source_disjoint`` is held to it, and every other manifest keeps the
pair-level verdict it always had, with the shared sources still reported in ``shared_sources``.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from lpq_tools.data.queries import (
    SELECTION_SOURCE_DISJOINT,
    QueryManifest,
    read_manifest,
)


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
    #: Source nodes both manifests use. Reported at every level; only ``level="source"`` fails on it.
    shared_sources: int = 0
    #: The rule that produced ``status``: "pair" (legacy) or "source".
    level: str = "pair"

    @property
    def disjoint(self) -> bool:
        return self.status == "DISJOINT"

    def as_dict(self) -> dict[str, object]:
        return dict(self.__dict__)


def compare_manifests(
    training: QueryManifest,
    evaluation: QueryManifest,
    train_name: str,
    eval_name: str,
    *,
    level: str = "pair",
) -> SplitReport:
    if level not in ("pair", "source"):
        raise ValueError(f"level must be 'pair' or 'source', got {level!r}")
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
            0,
            level,
        )
    train_pairs = {(p.source, p.target) for p in training.pairs}
    eval_pairs = {(p.source, p.target) for p in evaluation.pairs}
    shared = train_pairs & eval_pairs
    train_nodes = {n for p in train_pairs for n in p}
    eval_nodes = {n for p in eval_pairs for n in p}
    shared_nodes = train_nodes & eval_nodes
    shared_sources = {p.source for p in training.pairs} & {p.source for p in evaluation.pairs}
    overlapping = bool(shared) or (level == "source" and bool(shared_sources))
    status = "OVERLAPPING" if overlapping else "DISJOINT"
    if shared:
        detail = f"{len(shared)} query pair(s) appear in both sets"
    elif level == "source" and shared_sources:
        detail = f"{len(shared_sources)} source node(s) appear in both sets (source-level check)"
    elif shared_sources:
        detail = (
            "no query pair appears in both sets "
            f"({len(shared_sources)} shared source node(s) reported; pair-level check)"
        )
    else:
        detail = "no query pair appears in both sets"
    return SplitReport(
        status,
        train_name,
        eval_name,
        len(training.pairs),
        len(evaluation.pairs),
        len(shared),
        len(shared_nodes),
        detail,
        len(shared_sources),
        level,
    )


def check_split(
    training_path: Path, evaluation_path: Path, *, level: str | None = None
) -> SplitReport:
    """Compares two manifest files.

    With ``level=None`` the rule follows the training manifest: one that declares
    ``source_disjoint`` selection is checked at source level, any other at pair level.
    """
    training = read_manifest(training_path)
    if level is None:
        level = "source" if training.selection == SELECTION_SOURCE_DISJOINT else "pair"
    return compare_manifests(
        training,
        read_manifest(evaluation_path),
        training_path.name,
        evaluation_path.name,
        level=level,
    )
