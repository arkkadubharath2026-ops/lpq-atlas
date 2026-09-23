"""Structural validation of raw dataset files -- ESD.md section 26's "validate".

The converter validates too, and more strictly: it is the component that decides
whether a graph can be built at all. So why check first?

Because the two answer different questions. The converter asks "can I build a
CSR graph from this?" and answers with the first fatal problem it meets. This
module asks "is this the file we think it is?" and answers with a description of
the whole file: how many nodes and edges, whether weights are present, whether
the declared header agrees with the records that follow, how many self-loops and
duplicate pairs there are.

That description is what catches the failure mode the converter cannot see -- a
download that succeeded, parsed, and converted perfectly, but was the wrong
graph. A DIMACS distance file and the corresponding travel-time file have
identical structure and different meanings, and only the summary distinguishes
them.

Validation is a single streaming pass and never loads the file into memory.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

#: Problems are collected rather than raised one at a time, but a file with
#: millions of them should not produce millions of strings.
_MAX_REPORTED_PROBLEMS = 20

#: How many leading records to retain when testing whether the file lists both
#: directions of each edge. Bounded so that validating a 60-million-arc road
#: network does not need a set of 60 million tuples; large enough that the
#: answer is unambiguous, since the property is global rather than local.
_RECIPROCITY_SAMPLE = 200_000


@dataclass
class ValidationReport:
    """What a pass over a raw file found."""

    path: Path
    source_format: str

    records: int = 0
    distinct_nodes: int = 0
    min_node_id: int | None = None
    max_node_id: int | None = None

    self_loops: int = 0
    weighted_records: int = 0

    #: Records, within a bounded prefix, whose unordered endpoint pair had
    #: already been seen. A file that lists both directions of every undirected
    #: edge repeats each pair exactly once, so this lands near half the sample;
    #: a file that lists each edge once leaves it near zero.
    #:
    #: Counting repeats of the UNORDERED pair rather than looking for each
    #: record's reverse matters: the reverse of an edge need not sit near it in
    #: the file, so a "does its reverse appear?" test measures how well sorted
    #: the file is as much as how it lists edges.
    repeated_pairs: int = 0
    pair_sample: int = 0

    min_weight: float | None = None
    max_weight: float | None = None

    #: What a DIMACS "p sp" line declared, when there was one.
    declared_nodes: int | None = None
    declared_arcs: int | None = None

    problems: list[str] = field(default_factory=list)
    truncated_problem_list: bool = False

    @property
    def ok(self) -> bool:
        return not self.problems

    @property
    def edges_listed(self) -> str | None:
        """``"both"``, ``"once"`` or ``None`` when the sample is inconclusive.

        A file in which essentially every sampled record has a matching reverse
        lists both directions; one in which essentially none does lists each
        edge once. Anything between the two is reported as unknown rather than
        rounded to the nearer answer, because a partially reciprocal file is a
        genuinely directed graph and pairing it either way would be wrong.
        """
        if self.pair_sample < 1000:
            return None
        ratio = self.repeated_pairs / self.pair_sample
        if ratio > 0.40:
            return "both"
        if ratio < 0.01:
            return "once"
        return None

    def add_problem(self, message: str) -> None:
        if len(self.problems) < _MAX_REPORTED_PROBLEMS:
            self.problems.append(message)
        else:
            self.truncated_problem_list = True

    def to_dict(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "source_format": self.source_format,
            "records": self.records,
            "distinct_nodes": self.distinct_nodes,
            "min_node_id": self.min_node_id,
            "max_node_id": self.max_node_id,
            "self_loops": self.self_loops,
            "weighted_records": self.weighted_records,
            "min_weight": self.min_weight,
            "max_weight": self.max_weight,
            "declared_nodes": self.declared_nodes,
            "declared_arcs": self.declared_arcs,
            "ok": self.ok,
            "problems": list(self.problems),
            "truncated_problem_list": self.truncated_problem_list,
        }


def _note_weight(report: ValidationReport, weight: float) -> None:
    report.weighted_records += 1
    if report.min_weight is None or weight < report.min_weight:
        report.min_weight = weight
    if report.max_weight is None or weight > report.max_weight:
        report.max_weight = weight


def _note_node(report: ValidationReport, seen: set[int], node: int) -> None:
    seen.add(node)
    if report.min_node_id is None or node < report.min_node_id:
        report.min_node_id = node
    if report.max_node_id is None or node > report.max_node_id:
        report.max_node_id = node


def validate_snap_edge_list(path: Path) -> ValidationReport:
    """Validates a SNAP-style ``u v [w]`` edge list."""
    report = ValidationReport(path=path, source_format="snap")
    seen: set[int] = set()
    sampled_pairs: set[tuple[int, int]] = set()

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped or stripped[0] in "#%":
                continue

            fields = stripped.split()
            if len(fields) < 2:
                report.add_problem(f"line {number}: expected at least two node ids")
                continue
            try:
                u = int(fields[0])
                v = int(fields[1])
            except ValueError:
                report.add_problem(f"line {number}: node ids are not integers")
                continue
            if u < 0 or v < 0:
                report.add_problem(f"line {number}: negative node id")
                continue

            if len(fields) >= 3:
                try:
                    weight = float(fields[2])
                except ValueError:
                    report.add_problem(f"line {number}: weight is not a number")
                    continue
                if weight < 0.0:
                    report.add_problem(f"line {number}: negative weight {weight}")
                    continue
                _note_weight(report, weight)

            report.records += 1
            _note_node(report, seen, u)
            _note_node(report, seen, v)
            if u == v:
                # A self-loop is its own reverse, so including it would bias
                # the ratio towards "both".
                report.self_loops += 1
            elif report.pair_sample < _RECIPROCITY_SAMPLE:
                report.pair_sample += 1
                key = (u, v) if u < v else (v, u)
                if key in sampled_pairs:
                    report.repeated_pairs += 1
                else:
                    sampled_pairs.add(key)

    report.distinct_nodes = len(seen)
    if report.records == 0:
        report.add_problem("file contains no edge records")
    if 0 < report.weighted_records < report.records:
        # Half-weighted files are the shape that produces a graph where some
        # edges cost their real weight and the rest cost the default, which no
        # later step can detect.
        report.add_problem(
            f"only {report.weighted_records} of {report.records} records carry a "
            "weight; the rest would silently take the default"
        )
    return report


def validate_dimacs_graph(path: Path) -> ValidationReport:
    """Validates a DIMACS shortest-path ``.gr`` file."""
    report = ValidationReport(path=path, source_format="dimacs")
    seen: set[int] = set()
    saw_problem_line = False

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped or stripped[0] == "c":
                continue

            fields = stripped.split()
            kind = fields[0]

            if kind == "p":
                if saw_problem_line:
                    report.add_problem(f"line {number}: a second 'p' line")
                    continue
                saw_problem_line = True
                if len(fields) != 4 or fields[1] != "sp":
                    report.add_problem(
                        f"line {number}: expected 'p sp <nodes> <arcs>', got '{stripped}'"
                    )
                    continue
                try:
                    report.declared_nodes = int(fields[2])
                    report.declared_arcs = int(fields[3])
                except ValueError:
                    report.add_problem(f"line {number}: 'p sp' counts are not integers")
                continue

            if kind != "a":
                report.add_problem(f"line {number}: unexpected record type '{kind}'")
                continue

            if len(fields) != 4:
                report.add_problem(f"line {number}: expected 'a <u> <v> <w>'")
                continue
            try:
                u = int(fields[1])
                v = int(fields[2])
                weight = float(fields[3])
            except ValueError:
                report.add_problem(f"line {number}: malformed arc record")
                continue

            # DIMACS node ids are 1-based; a zero would become -1 after the
            # shift the reader applies.
            if u < 1 or v < 1:
                report.add_problem(f"line {number}: node id below 1 (DIMACS ids are 1-based)")
                continue
            if weight < 0.0:
                report.add_problem(f"line {number}: negative weight {weight}")
                continue

            report.records += 1
            _note_weight(report, weight)
            _note_node(report, seen, u)
            _note_node(report, seen, v)
            if u == v:
                report.self_loops += 1

    report.distinct_nodes = len(seen)

    if not saw_problem_line:
        report.add_problem("no 'p sp' problem line")
    if report.declared_arcs is not None and report.declared_arcs != report.records:
        report.add_problem(
            f"the header declares {report.declared_arcs} arcs but the file holds "
            f"{report.records}"
        )
    if report.declared_nodes is not None and report.max_node_id is not None:
        if report.max_node_id > report.declared_nodes:
            report.add_problem(
                f"node id {report.max_node_id} exceeds the declared node count "
                f"{report.declared_nodes}"
            )
    return report


def validate_dimacs_coordinates(path: Path, expected_nodes: int | None = None) -> ValidationReport:
    """Validates a DIMACS ``.co`` coordinate file."""
    report = ValidationReport(path=path, source_format="dimacs_coordinates")
    seen: set[int] = set()

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped or stripped[0] in "cp":
                continue
            fields = stripped.split()
            if fields[0] != "v":
                report.add_problem(f"line {number}: unexpected record type '{fields[0]}'")
                continue
            if len(fields) != 4:
                report.add_problem(f"line {number}: expected 'v <id> <x> <y>'")
                continue
            try:
                node = int(fields[1])
                float(fields[2])
                float(fields[3])
            except ValueError:
                report.add_problem(f"line {number}: malformed coordinate record")
                continue
            if node < 1:
                report.add_problem(f"line {number}: node id below 1")
                continue
            if node in seen:
                report.add_problem(f"line {number}: node {node} has a second coordinate")
                continue

            report.records += 1
            _note_node(report, seen, node)

    report.distinct_nodes = len(seen)
    if expected_nodes is not None and report.distinct_nodes != expected_nodes:
        # A partial coordinate set is worse than none: A* would run with a
        # heuristic that is zero for the missing nodes, which is admissible but
        # silently turns the search back into Dijkstra in those regions.
        report.add_problem(
            f"coordinates cover {report.distinct_nodes} nodes but the graph has "
            f"{expected_nodes}"
        )
    return report


def validate_raw_file(
    path: Path, source_format: str, *, expected_nodes: int | None = None
) -> ValidationReport:
    """Dispatches to the validator for *source_format*."""
    if source_format == "snap":
        return validate_snap_edge_list(path)
    if source_format == "dimacs":
        return validate_dimacs_graph(path)
    if source_format == "dimacs_coordinates":
        return validate_dimacs_coordinates(path, expected_nodes)
    raise ValueError(f"no validator for source format '{source_format}'")
