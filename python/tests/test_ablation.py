"""Tests for the ablation waterfall.

Two things are worth testing here, and neither is the arithmetic.

A waterfall is a *sequence*: every ratio is relative to the rung above, so the
order of the ladder is load-bearing. A table that silently reordered or dropped
a rung would still print, still look like a waterfall, and attribute the wrong
cost to the wrong step.

And a missing rung must stay visible. The common case is `lpq_model` absent
because no model was supplied; a table that quietly shortened itself would read
as a complete attribution of a system that had not been fully measured.
"""

from __future__ import annotations

import json

import pytest

from lpq_tools.evaluation.ablation import (
    WATERFALL,
    build_waterfall,
    format_waterfall,
)
from lpq_tools.evaluation.results import ResultsError, read_results


def write_results(path, rows):
    """Writes a minimal results file in the runner's schema."""
    records = []
    for pq, source, comparisons, median in rows:
        records.append(
            {
                "dataset": "test",
                "algorithm": "dijkstra",
                "pq": pq,
                "mode": "exact",
                "source": source,
                "target": source + 1000,
                "supported": True,
                "timing": {
                    "median_ns": median,
                    "relative_mad": 0.01,
                    "noisy": False,
                    "cpu_pinned": True,
                },
                "pq_operations": {"comparisons": comparisons, "pops": 10},
            }
        )
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "experiment_id": "test",
                "generated_at": "now",
                "records": records,
            }
        ),
        encoding="utf-8",
    )
    return path


@pytest.fixture
def full_ladder(tmp_path):
    return write_results(
        tmp_path / "r.json",
        [
            ("binary_heap", 1, 1000, 100.0),
            ("binary_heap", 2, 1000, 100.0),
            ("radix_heap", 1, 500, 130.0),
            ("radix_heap", 2, 500, 130.0),
            ("lpq", 1, 1200, 500.0),
            ("lpq", 2, 1200, 500.0),
            ("lpq_model", 1, 1300, 520.0),
            ("lpq_model", 2, 1300, 520.0),
        ],
    )


# -----------------------------------------------------------------------------
# The ladder is a sequence
# -----------------------------------------------------------------------------


def test_steps_come_back_in_ladder_order(full_ladder):
    steps = build_waterfall(read_results(full_ladder))

    assert [s.configuration for s in steps] == [name for name, _ in WATERFALL]


def test_each_ratio_is_against_the_rung_above(full_ladder):
    steps = {s.configuration: s for s in build_waterfall(read_results(full_ladder))}

    # radix does half the comparisons of binary; lpq does 2400/1000 = 2.4x radix.
    assert steps["radix_heap"].comparison_ratio == pytest.approx(1000 / 2000)
    assert steps["lpq"].comparison_ratio == pytest.approx(2400 / 1000)
    assert steps["lpq_model"].comparison_ratio == pytest.approx(2600 / 2400)


def test_the_base_column_is_against_the_first_rung(full_ladder):
    steps = {s.configuration: s for s in build_waterfall(read_results(full_ladder))}

    assert steps["lpq"].comparison_ratio_to_base == pytest.approx(2400 / 2000)
    assert steps["lpq_model"].comparison_ratio_to_base == pytest.approx(2600 / 2000)


def test_comparisons_are_summed_across_queries(full_ladder):
    steps = {s.configuration: s for s in build_waterfall(read_results(full_ladder))}

    # Two queries at 1000 each. Exact quantities, so this is an equality.
    assert steps["binary_heap"].comparisons == 2000


# -----------------------------------------------------------------------------
# A missing rung stays visible
# -----------------------------------------------------------------------------


def test_an_absent_configuration_is_reported_not_dropped(tmp_path):
    path = write_results(
        tmp_path / "partial.json",
        [("binary_heap", 1, 1000, 100.0), ("lpq", 1, 1200, 500.0)],
    )
    steps = build_waterfall(read_results(path))

    assert len(steps) == len(WATERFALL)
    missing = {s.configuration for s in steps if not s.measured}
    assert missing == {"radix_heap", "lpq_model"}


def test_a_missing_rung_carries_a_reason(tmp_path):
    path = write_results(tmp_path / "partial.json", [("binary_heap", 1, 1000, 100.0)])
    steps = {s.configuration: s for s in build_waterfall(read_results(path))}

    assert steps["lpq_model"].reason != ""


def test_a_missing_rung_appears_in_the_rendered_table(tmp_path):
    path = write_results(tmp_path / "partial.json", [("binary_heap", 1, 1000, 100.0)])
    rendered = format_waterfall(build_waterfall(read_results(path)))

    assert "lpq_model" in rendered
    assert "not measured" in rendered


def test_ratios_skip_an_unmeasured_rung_rather_than_dividing_by_zero(tmp_path):
    # radix absent: lpq's "vs prev" must be against binary_heap, not against a
    # zero that would make the ratio infinite or nan.
    path = write_results(
        tmp_path / "gap.json",
        [("binary_heap", 1, 1000, 100.0), ("lpq", 1, 3000, 500.0)],
    )
    steps = {s.configuration: s for s in build_waterfall(read_results(path))}

    assert steps["lpq"].comparison_ratio == pytest.approx(3.0)


def test_refuses_a_file_with_no_ladder_configuration_at_all(tmp_path):
    path = write_results(tmp_path / "other.json", [("pairing_heap", 1, 1000, 100.0)])

    with pytest.raises(ResultsError, match="waterfall"):
        build_waterfall(read_results(path))


# -----------------------------------------------------------------------------
# The rendered table does not overclaim
# -----------------------------------------------------------------------------


def test_the_table_says_comparisons_are_exact_and_timings_are_not(full_ladder):
    rendered = format_waterfall(build_waterfall(read_results(full_ladder)))

    assert "EXACT" in rendered
    # The project has measured a queue doing fewer comparisons while running
    # slower, so the table must not let a timing ratio read as a speedup.
    assert "not a speedup claim" in rendered


def test_noise_flags_attach_to_the_row_that_earned_them(tmp_path):
    path = tmp_path / "noisy.json"
    write_results(path, [("binary_heap", 1, 1000, 100.0), ("lpq", 1, 1200, 500.0)])
    payload = json.loads(path.read_text(encoding="utf-8"))
    payload["records"][1]["timing"]["noisy"] = True
    path.write_text(json.dumps(payload), encoding="utf-8")

    steps = {s.configuration: s for s in build_waterfall(read_results(path))}

    assert not steps["binary_heap"].noisy
    assert steps["lpq"].noisy


# -----------------------------------------------------------------------------
# Results reading
# -----------------------------------------------------------------------------


def test_refuses_an_unreadable_schema_version(tmp_path):
    path = tmp_path / "future.json"
    path.write_text(json.dumps({"schema_version": 99, "records": []}), encoding="utf-8")

    with pytest.raises(ResultsError, match="schema version"):
        read_results(path)


def test_pairs_queries_across_configurations(full_ladder):
    results = read_results(full_ladder)
    left, right = results.paired_medians("binary_heap", "lpq")

    assert len(left) == len(right) == 2


def test_refuses_to_pair_configurations_with_no_shared_query(tmp_path):
    # An unpaired comparison over a road network's query distribution is
    # dominated by which queries happened to be included, so an empty
    # intersection must fail rather than silently compare nothing.
    path = write_results(
        tmp_path / "disjoint.json",
        [("binary_heap", 1, 1000, 100.0), ("lpq", 2, 1200, 500.0)],
    )

    with pytest.raises(ResultsError, match="nothing"):
        read_results(path).paired_medians("binary_heap", "lpq")
