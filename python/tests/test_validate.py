"""Tests for raw-file validation.

Validation exists to catch the file that downloads cleanly, parses cleanly, and
is the wrong graph. So the tests check that the summary it produces is accurate
as well as that the malformed cases are rejected -- an inaccurate summary would
be worse than none, because it would be trusted.
"""

from __future__ import annotations

import pytest

from lpq_tools.data.validate import validate_raw_file


def write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


# -----------------------------------------------------------------------------
# SNAP edge lists
# -----------------------------------------------------------------------------


def test_snap_summary_is_accurate(tmp_path):
    path = write(
        tmp_path,
        "edges.txt",
        "# a comment\n% another comment\n\n10 20\n20 30\n30 10\n",
    )
    report = validate_raw_file(path, "snap")
    assert report.ok, report.problems
    assert report.records == 3
    assert report.distinct_nodes == 3
    assert report.min_node_id == 10
    assert report.max_node_id == 30
    assert report.self_loops == 0
    assert report.weighted_records == 0


def test_snap_counts_self_loops_without_rejecting_them(tmp_path):
    # Self-loops are harmless for Dijkstra with non-negative weights, so they
    # are reported rather than treated as corruption.
    path = write(tmp_path, "edges.txt", "1 1\n1 2\n")
    report = validate_raw_file(path, "snap")
    assert report.ok
    assert report.self_loops == 1


def test_snap_reads_optional_weights(tmp_path):
    path = write(tmp_path, "edges.txt", "1 2 5.5\n2 3 1.5\n")
    report = validate_raw_file(path, "snap")
    assert report.ok
    assert report.weighted_records == 2
    assert report.min_weight == pytest.approx(1.5)
    assert report.max_weight == pytest.approx(5.5)


def test_snap_rejects_a_partially_weighted_file(tmp_path):
    # The shape that produces a graph where some edges cost their real weight
    # and the rest silently take the default.
    path = write(tmp_path, "edges.txt", "1 2 5\n2 3\n3 4 7\n")
    report = validate_raw_file(path, "snap")
    assert not report.ok
    assert any("weight" in p for p in report.problems)


def test_snap_rejects_a_negative_weight(tmp_path):
    path = write(tmp_path, "edges.txt", "1 2 -3\n")
    report = validate_raw_file(path, "snap")
    assert not report.ok
    assert any("negative weight" in p for p in report.problems)


def test_snap_rejects_a_truncated_record(tmp_path):
    path = write(tmp_path, "edges.txt", "1 2\n3\n")
    report = validate_raw_file(path, "snap")
    assert not report.ok


def test_snap_rejects_a_non_integer_node_id(tmp_path):
    path = write(tmp_path, "edges.txt", "1 two\n")
    report = validate_raw_file(path, "snap")
    assert not report.ok


def test_snap_rejects_an_empty_file(tmp_path):
    path = write(tmp_path, "edges.txt", "# only comments\n")
    report = validate_raw_file(path, "snap")
    assert not report.ok
    assert any("no edge records" in p for p in report.problems)


def test_the_problem_list_is_capped(tmp_path):
    # A wholly corrupt file must not produce a million strings.
    path = write(tmp_path, "edges.txt", "\n".join("bad" for _ in range(500)))
    report = validate_raw_file(path, "snap")
    assert not report.ok
    assert len(report.problems) <= 20
    assert report.truncated_problem_list


# -----------------------------------------------------------------------------
# DIMACS
# -----------------------------------------------------------------------------


def test_dimacs_summary_is_accurate(tmp_path):
    path = write(
        tmp_path,
        "graph.gr",
        "c comment\np sp 4 4\na 1 2 10\na 2 1 10\na 2 3 20\na 3 2 20\n",
    )
    report = validate_raw_file(path, "dimacs")
    assert report.ok, report.problems
    assert report.declared_nodes == 4
    assert report.declared_arcs == 4
    assert report.records == 4
    assert report.min_weight == pytest.approx(10.0)
    assert report.max_weight == pytest.approx(20.0)


def test_dimacs_rejects_an_arc_count_that_disagrees_with_the_header(tmp_path):
    # The single most valuable check here: a truncated download parses fine and
    # produces a smaller, entirely valid graph.
    path = write(tmp_path, "graph.gr", "p sp 3 4\na 1 2 10\na 2 3 20\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok
    assert any("declares 4 arcs" in p for p in report.problems)


def test_dimacs_rejects_a_missing_problem_line(tmp_path):
    path = write(tmp_path, "graph.gr", "a 1 2 10\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok
    assert any("problem line" in p for p in report.problems)


def test_dimacs_rejects_a_zero_node_id(tmp_path):
    # DIMACS ids are 1-based; a zero becomes -1 after the reader's shift.
    path = write(tmp_path, "graph.gr", "p sp 2 1\na 0 1 5\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok
    assert any("1-based" in p for p in report.problems)


def test_dimacs_rejects_a_node_id_above_the_declared_count(tmp_path):
    path = write(tmp_path, "graph.gr", "p sp 2 1\na 1 9 5\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok
    assert any("exceeds the declared node count" in p for p in report.problems)


def test_dimacs_rejects_a_negative_weight(tmp_path):
    path = write(tmp_path, "graph.gr", "p sp 2 1\na 1 2 -5\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok


def test_dimacs_rejects_an_unknown_record_type(tmp_path):
    path = write(tmp_path, "graph.gr", "p sp 2 1\nx 1 2 5\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok


def test_dimacs_rejects_a_second_problem_line(tmp_path):
    path = write(tmp_path, "graph.gr", "p sp 2 1\np sp 3 2\na 1 2 5\n")
    report = validate_raw_file(path, "dimacs")
    assert not report.ok


# -----------------------------------------------------------------------------
# DIMACS coordinates
# -----------------------------------------------------------------------------


def test_coordinates_summary_is_accurate(tmp_path):
    path = write(tmp_path, "graph.co", "c comment\np aux sp co 3\nv 1 10 20\nv 2 30 40\nv 3 50 60\n")
    report = validate_raw_file(path, "dimacs_coordinates", expected_nodes=3)
    assert report.ok, report.problems
    assert report.records == 3
    assert report.distinct_nodes == 3


def test_coordinates_reject_a_partial_set(tmp_path):
    # A partial coordinate set is worse than none: A* stays admissible but
    # silently degenerates to Dijkstra wherever coordinates are missing.
    path = write(tmp_path, "graph.co", "v 1 10 20\nv 2 30 40\n")
    report = validate_raw_file(path, "dimacs_coordinates", expected_nodes=5)
    assert not report.ok
    assert any("cover 2 nodes" in p for p in report.problems)


def test_coordinates_reject_a_duplicate_node(tmp_path):
    path = write(tmp_path, "graph.co", "v 1 10 20\nv 1 30 40\n")
    report = validate_raw_file(path, "dimacs_coordinates")
    assert not report.ok
    assert any("second coordinate" in p for p in report.problems)


def test_an_unknown_format_is_rejected_rather_than_guessed(tmp_path):
    path = write(tmp_path, "x.txt", "1 2\n")
    with pytest.raises(ValueError, match="no validator"):
        validate_raw_file(path, "graphml")


# -----------------------------------------------------------------------------
# Edge-listing convention
# -----------------------------------------------------------------------------
#
# Regression. SNAP is not consistent about whether an undirected edge appears
# once or in both directions, and the file says nothing either way:
# facebook_combined.txt holds 88,234 records for 88,234 published edges, while
# roadNet-PA.txt holds 3,083,796 for 1,541,898. Reading the second as though it
# were the first produced a graph with exactly twice the edges -- structurally
# valid, silent, and wrong in every per-edge measurement taken on it.


def test_a_singly_listed_file_is_detected(tmp_path):
    path = write(tmp_path, "once.txt", "".join(f"{i} {i + 1}\n" for i in range(2000)))
    report = validate_raw_file(path, "snap")
    assert report.ok
    assert report.edges_listed == "once"
    assert report.repeated_pairs == 0


def test_a_doubly_listed_file_is_detected(tmp_path):
    body = "".join(f"{i} {i + 1}\n{i + 1} {i}\n" for i in range(2000))
    path = write(tmp_path, "both.txt", body)
    report = validate_raw_file(path, "snap")
    assert report.ok
    assert report.edges_listed == "both"
    # Every pair appears exactly twice, so half the sampled records repeat one.
    assert report.repeated_pairs == report.pair_sample // 2


def test_a_partially_reciprocal_file_is_reported_as_unknown(tmp_path):
    # A genuinely directed graph that happens to be partly symmetric must not be
    # rounded to either convention: pairing it either way would be wrong.
    lines = []
    for i in range(2000):
        lines.append(f"{i} {i + 1}\n")
        if i % 2 == 0:
            lines.append(f"{i + 1} {i}\n")
    path = write(tmp_path, "mixed.txt", "".join(lines))
    report = validate_raw_file(path, "snap")
    assert report.edges_listed is None


def test_a_tiny_file_is_inconclusive_rather_than_guessed(tmp_path):
    # Too small a sample cannot distinguish the conventions, and a guess here
    # would be worse than an admission of ignorance.
    path = write(tmp_path, "tiny.txt", "1 2\n2 1\n")
    report = validate_raw_file(path, "snap")
    assert report.edges_listed is None


def test_self_loops_do_not_bias_the_detection(tmp_path):
    # A self-loop is its own reverse, so counting it would push a singly-listed
    # file towards "both".
    body = "".join(f"{i} {i}\n{i} {i + 1}\n" for i in range(2000))
    path = write(tmp_path, "loops.txt", body)
    report = validate_raw_file(path, "snap")
    assert report.self_loops == 2000
    assert report.edges_listed == "once"
