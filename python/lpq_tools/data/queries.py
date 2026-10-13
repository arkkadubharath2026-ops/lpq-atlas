"""Query manifest generation -- ESD.md section 26's "generate query manifest".

PRD.md section 18:

    Source-target pairs should be generated deterministically from fixed seeds
    and stored as experiment manifests.

and, in the same section, *do not cherry-pick favorable queries*. Both are
properties of this module.

-------------------------------------------------------------------------------
Why the generator is written out rather than taken from the standard library
-------------------------------------------------------------------------------

``random.Random`` and ``numpy.random.Generator`` are both reproducible in
practice, but their guarantees are about the stream, not about how a particular
helper consumes it: ``random.sample`` and ``Generator.choice`` have changed
their internal consumption patterns across versions before. A manifest that
silently regenerated to different pairs after an interpreter upgrade would
invalidate every paired comparison in the results without failing anything.

SplitMix64 is twelve lines, is fully specified by those lines, and produces the
same stream on every Python that has integers. The cost of writing it out is
much smaller than the cost of not being able to trust a manifest.

-------------------------------------------------------------------------------
Reachability
-------------------------------------------------------------------------------

Pairs are drawn uniformly and are NOT filtered for reachability, because
filtering would require running the search that the manifest exists to measure,
and because discarding the hard pairs is exactly the cherry-picking PRD.md
section 18 forbids. A real dataset has a giant component and a scattering of
small ones; an unreachable pair is a legitimate query whose answer is "no path",
and the benchmark runner records it as such.
"""

from __future__ import annotations

from collections.abc import Collection, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

_MASK64 = (1 << 64) - 1
_GOLDEN = 0x9E3779B97F4A7C15


class SplitMix64:
    """A small, fully specified 64-bit generator.

    Chosen over the standard library for reproducibility across interpreter
    versions; see the module docstring.
    """

    __slots__ = ("_state",)

    def __init__(self, seed: int) -> None:
        self._state = seed & _MASK64

    def next_u64(self) -> int:
        self._state = (self._state + _GOLDEN) & _MASK64
        z = self._state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _MASK64
        return z ^ (z >> 31)

    def next_below(self, bound: int) -> int:
        """Uniform on ``[0, bound)``, without modulo bias.

        Lemire's method: reject the low residue window rather than folding it,
        so every value in the range is equally likely. The bias a plain modulo
        would introduce is tiny for the ranges here, but "tiny and
        direction-dependent" is exactly the kind of thing that ends up
        correlating with node id, and node id correlates with position in a road
        network.
        """
        if bound <= 0:
            raise ValueError("bound must be positive")
        threshold = (-bound) % bound
        while True:
            value = self.next_u64()
            if value >= threshold:
                return value % bound


def derive_seed(master: int, label: str) -> int:
    """Derives an independent sub-seed from a master seed and a label.

    Lets one seed in a dataset config drive several independent streams (query
    sources, query targets, a future perturbation schedule) without any of them
    sharing state, and without the config having to name a seed per stream.
    """
    h = master & _MASK64
    for byte in label.encode("utf-8"):
        h = ((h ^ byte) * 0x100000001B3) & _MASK64
        h = (h + _GOLDEN) & _MASK64
    generator = SplitMix64(h)
    return generator.next_u64()


#: The default selection rule: sources and targets uniform over all nodes.
SELECTION_UNIFORM = "uniform_pairs_distinct_endpoints"

#: Sources are uniform over the nodes NOT in an excluded set; targets are unchanged.
SELECTION_SOURCE_DISJOINT = "uniform_pairs_distinct_endpoints_source_disjoint"


@dataclass(frozen=True)
class QueryPair:
    source: int
    target: int


@dataclass(frozen=True)
class QueryManifest:
    """A deterministic set of source-target pairs for one dataset."""

    dataset_id: str
    node_count: int
    seed: int
    pairs: tuple[QueryPair, ...]

    #: The checksum of the graph the pairs were generated against. A manifest is
    #: only meaningful for one conversion: node ids are dense indices assigned
    #: during conversion, so the same pair means a different query after the
    #: graph is rebuilt from a different source file.
    graph_checksum: int | None = None

    #: How the pairs were selected. Descriptive only (the engine's manifest reader ignores the
    #: value), but it is the one place a manifest can say that its sources were constrained.
    selection: str = SELECTION_UNIFORM

    def to_dict(self) -> dict[str, Any]:
        return {
            "dataset_id": self.dataset_id,
            "node_count": self.node_count,
            "seed": self.seed,
            "graph_checksum_fnv1a64": self.graph_checksum,
            "query_count": len(self.pairs),
            "generator": "splitmix64",
            "selection": self.selection,
            "reachability_filtered": False,
            "queries": [{"source": p.source, "target": p.target} for p in self.pairs],
        }


def generate_query_pairs(
    node_count: int,
    count: int,
    seed: int,
    *,
    exclude_sources: Collection[int] | None = None,
) -> tuple[QueryPair, ...]:
    """Draws *count* uniform source-target pairs over ``[0, node_count)``.

    Endpoints of a pair are distinct: a query from a node to itself has the
    answer zero without touching the priority queue, so including them would
    dilute the measurement with non-queries.

    Duplicate PAIRS are allowed. Rejecting them would make the draw
    non-uniform, and on a small graph with a large query count it could not
    terminate at all.

    *exclude_sources* removes nodes from the pool the SOURCE is drawn from; targets are
    still drawn from every node other than the source. Training labels come from a full
    single-source search, so a training query that reuses a benchmark query's source reuses
    its search even when the pair differs; excluding the benchmark's sources is the
    separation that matches how labels are made. The source is drawn as an index into the
    sorted list of allowed nodes, so the number of draws per pair stays fixed, and with no
    exclusion the stream is exactly the one every committed manifest was generated from.
    """
    if node_count < 2:
        raise ValueError(
            f"cannot generate source-target pairs on a graph with {node_count} node(s)"
        )
    if count <= 0:
        raise ValueError("query count must be positive")

    allowed: list[int] | None = None
    if exclude_sources:
        excluded = set(exclude_sources)
        outside = sorted(u for u in excluded if not 0 <= u < node_count)
        if outside:
            raise ValueError(
                f"excluded source ids outside [0, {node_count}): {outside[:5]}"
                " -- a manifest of a different graph?"
            )
        allowed = [u for u in range(node_count) if u not in excluded]
        if not allowed:
            raise ValueError("every node is excluded, so there is no source left to draw")

    rng = SplitMix64(derive_seed(seed, "queries"))
    pairs: list[QueryPair] = []
    for _ in range(count):
        if allowed is None:
            source = rng.next_below(node_count)
        else:
            source = allowed[rng.next_below(len(allowed))]
        target = rng.next_below(node_count - 1)
        # Map the target into "anything but source" without a rejection loop, so
        # the number of draws per pair is fixed and the stream stays aligned
        # regardless of which sources come up.
        if target >= source:
            target += 1
        pairs.append(QueryPair(source, target))
    return tuple(pairs)


def build_manifest(
    dataset_id: str,
    node_count: int,
    *,
    count: int,
    seed: int,
    graph_checksum: int | None = None,
    exclude_sources: Collection[int] | None = None,
) -> QueryManifest:
    return QueryManifest(
        dataset_id=dataset_id,
        node_count=node_count,
        seed=seed,
        pairs=generate_query_pairs(node_count, count, seed, exclude_sources=exclude_sources),
        graph_checksum=graph_checksum,
        selection=SELECTION_SOURCE_DISJOINT if exclude_sources else SELECTION_UNIFORM,
    )


def shared_sources(left: QueryManifest, right: QueryManifest) -> set[int]:
    """The source nodes both manifests use.

    Sources, not pairs: training labels come from a full single-source search, so a shared source
    means a shared search whatever the targets are.
    """
    return {p.source for p in left.pairs} & {p.source for p in right.pairs}


def write_manifest(
    path: Path, manifest: QueryManifest, *, notes: Iterable[str] = ()
) -> None:
    """Writes a manifest as YAML (ESD.md section 22's manifest format).

    *notes* become extra comment lines after the header. They are comments because the
    engine's reader rejects any top-level key it does not know.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    header = (
        "# Query manifest -- generated by lpq_tools.data.queries.\n"
        "#\n"
        "# Regenerate rather than edit: the pairs are a deterministic function of\n"
        "# the dataset's node count and seed, so a hand edit cannot be reproduced\n"
        "# and would break the paired comparison between queues.\n"
    )
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(header)
        for note in notes:
            handle.write(f"# {note}\n")
        yaml.safe_dump(manifest.to_dict(), handle, sort_keys=False, default_flow_style=False)


def read_manifest(path: Path) -> QueryManifest:
    with path.open("r", encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    return QueryManifest(
        dataset_id=raw["dataset_id"],
        node_count=int(raw["node_count"]),
        seed=int(raw["seed"]),
        pairs=tuple(QueryPair(int(q["source"]), int(q["target"])) for q in raw["queries"]),
        graph_checksum=(
            None
            if raw.get("graph_checksum_fnv1a64") is None
            else int(raw["graph_checksum_fnv1a64"])
        ),
        selection=str(raw.get("selection", SELECTION_UNIFORM)),
    )
