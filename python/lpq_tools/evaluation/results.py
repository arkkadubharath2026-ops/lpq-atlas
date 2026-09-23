"""Reading the benchmark runner's JSON results.

One place that knows the result schema, so a change to it breaks in one file
rather than in every analysis that ever read a results file.

The reader deliberately keeps per-query records rather than collapsing them on
load. Every comparison this project makes is **paired** -- the same query set
runs through every queue, and per-query difficulty on a road network varies by
orders of magnitude -- so an analysis that started from per-queue aggregates
would have thrown away the pairing before it began.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


class ResultsError(Exception):
    """Raised when a results file is malformed or from an unreadable schema."""


#: The schema this module understands. A mismatch is an error rather than a best
#: effort: fields move between versions, and reading a v2 file as a v1 one
#: produces numbers rather than a failure.
SCHEMA_VERSION = 1


@dataclass(frozen=True)
class QueryResult:
    """One query measured on one configuration."""

    dataset: str
    algorithm: str
    pq: str
    mode: str
    source: int
    target: int

    supported: bool
    unsupported_reason: str

    median_ns: float
    relative_mad: float
    noisy: bool
    cpu_pinned: bool

    comparisons: int
    pops: int

    @property
    def key(self) -> tuple[int, int]:
        """What makes two records the same query, for pairing."""
        return (self.source, self.target)


@dataclass(frozen=True)
class ResultSet:
    experiment_id: str
    generated_at: str
    records: list[QueryResult]

    def configurations(self) -> list[str]:
        """Configuration labels, in the order they were first measured.

        Order is preserved rather than sorted because the runner emits them in
        the order the matrix specified, and that order is usually meaningful --
        an ablation waterfall in particular is a sequence, not a set.
        """
        seen: list[str] = []
        for record in self.records:
            if record.pq not in seen:
                seen.append(record.pq)
        return seen

    def for_configuration(self, pq: str) -> list[QueryResult]:
        return [r for r in self.records if r.pq == pq]

    def paired_medians(self, a: str, b: str) -> tuple[list[float], list[float]]:
        """Per-query medians for two configurations, aligned by query.

        Queries measured for only one of the two are dropped, and dropping them
        is the right call: an unpaired comparison over a road network's query
        distribution is dominated by which queries happened to be included.

        Raises:
            ResultsError: if nothing is left to compare, which otherwise
                surfaces much later as an empty statistic.
        """
        left = {r.key: r.median_ns for r in self.for_configuration(a) if r.supported}
        right = {r.key: r.median_ns for r in self.for_configuration(b) if r.supported}
        shared = [k for k in left if k in right]
        if not shared:
            raise ResultsError(
                f"no query was measured on both '{a}' and '{b}', so there is nothing "
                "to compare"
            )
        return ([left[k] for k in shared], [right[k] for k in shared])

    def total_comparisons(self, pq: str) -> int:
        return sum(r.comparisons for r in self.for_configuration(pq) if r.supported)

    def total_pops(self, pq: str) -> int:
        return sum(r.pops for r in self.for_configuration(pq) if r.supported)


def _as_int(value) -> int:
    return int(value) if value is not None else 0


def read_results(path: str | Path) -> ResultSet:
    """Reads one results file.

    Raises:
        ResultsError: on an unreadable schema version or a missing records list.
    """
    payload = json.loads(Path(path).read_text(encoding="utf-8"))

    version = payload.get("schema_version")
    if version != SCHEMA_VERSION:
        raise ResultsError(
            f"results schema version {version} cannot be read here (expected "
            f"{SCHEMA_VERSION}); fields move between versions and reading one as "
            "the other yields numbers rather than an error"
        )
    if "records" not in payload:
        raise ResultsError(f"{path} carries no records list")

    records = []
    for raw in payload["records"]:
        timing = raw.get("timing") or {}
        operations = raw.get("pq_operations") or {}
        records.append(
            QueryResult(
                dataset=raw.get("dataset", ""),
                algorithm=raw.get("algorithm", ""),
                pq=raw.get("pq", ""),
                mode=raw.get("mode", ""),
                source=_as_int(raw.get("source")),
                target=_as_int(raw.get("target")),
                supported=bool(raw.get("supported", True)),
                unsupported_reason=raw.get("unsupported_reason") or "",
                median_ns=float(timing.get("median_ns") or 0.0),
                relative_mad=float(timing.get("relative_mad") or 0.0),
                noisy=bool(timing.get("noisy", False)),
                cpu_pinned=bool(timing.get("cpu_pinned", False)),
                comparisons=_as_int(operations.get("comparisons")),
                pops=_as_int(operations.get("pops")),
            )
        )

    return ResultSet(
        experiment_id=payload.get("experiment_id", ""),
        generated_at=payload.get("generated_at", ""),
        records=records,
    )
