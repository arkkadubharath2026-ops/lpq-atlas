"""Tests for the statistical analysis package.

The properties that matter are the ones a published claim would rest on:
the descriptive statistics are what they say they are, the randomised
procedures reproduce exactly from a seed, a paired test finds a real shift and
does not invent one, misaligned input is refused rather than truncated, and no
procedure ever quietly shrinks a sample.

All data here is constructed, not measured. Nothing in this file is a benchmark
result.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from lpq_tools.data.queries import SplitMix64
from lpq_tools.statistics.comparison import (
    CORRECTION_BENJAMINI_HOCHBERG,
    CORRECTION_HOLM,
    TEST_PERMUTATION,
    TEST_WILCOXON,
    adjust_comparisons,
    adjust_p_values,
    compare_paired,
    paired_permutation_test,
    wilcoxon_signed_rank_test,
)
from lpq_tools.statistics.summary import (
    bootstrap_mean_ci,
    bootstrap_median_ci,
    outlier_report,
    splitmix64_stream,
    summarise,
)


def _deterministic_noise(seed: int, count: int, scale: float) -> list[float]:
    """Zero-centred noise from the project's own generator, so tests never drift."""
    rng = SplitMix64(seed)
    values = []
    for _ in range(count):
        unit = (rng.next_u64() >> 11) / float(1 << 53)
        values.append((unit - 0.5) * 2.0 * scale)
    return values


# -----------------------------------------------------------------------------
# Descriptive statistics, against values that can be checked by hand
# -----------------------------------------------------------------------------


def test_summary_matches_hand_computed_values():
    # Nine points, deliberately arranged so every statistic is obvious:
    # sorted they are 1..8 plus a far-away 100.
    sample = [4.0, 1.0, 7.0, 3.0, 5.0, 2.0, 8.0, 6.0, 100.0]
    summary = summarise(sample)

    assert summary.count == 9
    assert summary.minimum == 1.0
    assert summary.maximum == 100.0
    assert summary.median == 5.0  # the fifth of nine sorted values

    # Deviations from the median of 5 are 4,3,2,1,0,1,2,3,95; sorted, the fifth
    # is 2.
    assert summary.mad == 2.0

    # The mean is dragged well above the median by the single large point, which
    # is precisely why the median is the headline statistic.
    assert summary.mean == pytest.approx((1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 100) / 9)
    assert summary.mean > summary.median

    assert summary.stddev == pytest.approx(float(np.std(np.asarray(sample), ddof=1)))
    assert summary.p25 == pytest.approx(3.0)
    assert summary.p75 == pytest.approx(7.0)
    assert summary.iqr == pytest.approx(4.0)
    assert summary.p95 == pytest.approx(float(np.percentile(sample, 95)))
    assert summary.p99 == pytest.approx(float(np.percentile(sample, 99)))


def test_summary_of_an_even_length_sample_interpolates_the_median():
    summary = summarise([1.0, 2.0, 3.0, 4.0])
    assert summary.median == 2.5
    # Deviations are 1.5, 0.5, 0.5, 1.5; their median is 1.0.
    assert summary.mad == 1.0


def test_summary_of_a_single_point_reports_zero_spread():
    summary = summarise([7.5])
    assert summary.count == 1
    assert summary.median == 7.5
    assert summary.mad == 0.0
    assert summary.stddev == 0.0


def test_summary_rejects_an_empty_sample():
    with pytest.raises(ValueError):
        summarise([])


def test_summary_rejects_a_non_finite_measurement():
    with pytest.raises(ValueError):
        summarise([1.0, 2.0, float("nan")])


# -----------------------------------------------------------------------------
# The deterministic stream underneath every randomised procedure
# -----------------------------------------------------------------------------


def test_vectorised_stream_matches_the_scalar_generator():
    # The vectorised form is an optimisation of lpq_tools.data.queries.SplitMix64
    # and must produce that exact stream, or every interval in the results
    # becomes irreproducible from the seed that is recorded with it.
    scalar = SplitMix64(20240607)
    expected = [scalar.next_u64() for _ in range(257)]
    produced = splitmix64_stream(20240607, 257)
    assert [int(value) for value in produced] == expected


# -----------------------------------------------------------------------------
# Bootstrap intervals
# -----------------------------------------------------------------------------


def test_bootstrap_median_ci_is_identical_for_the_same_seed():
    sample = [float(x) for x in range(1, 41)]
    first = bootstrap_median_ci(sample, seed=17, resamples=2000)
    second = bootstrap_median_ci(sample, seed=17, resamples=2000)
    assert first == second


def test_bootstrap_median_ci_with_a_different_seed_still_agrees_closely():
    sample = [float(x) for x in range(1, 41)]
    first = bootstrap_median_ci(sample, seed=17, resamples=4000)
    second = bootstrap_median_ci(sample, seed=18, resamples=4000)

    assert first != second  # a different stream is genuinely a different run
    assert first.point == second.point  # but the statistic itself is not random
    assert first.low == pytest.approx(second.low, abs=1.5)
    assert first.high == pytest.approx(second.high, abs=1.5)


def test_bootstrap_interval_brackets_the_point_estimate():
    sample = [float(x) for x in range(1, 41)]
    interval = bootstrap_median_ci(sample, seed=3, resamples=2000)
    assert interval.low <= interval.point <= interval.high
    assert interval.level == 0.95
    assert interval.resamples == 2000
    assert interval.seed == 3
    assert interval.statistic == "median"


def test_bootstrap_mean_ci_is_reproducible_and_reports_its_statistic():
    sample = [1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 30.0]
    first = bootstrap_mean_ci(sample, seed=99, resamples=2000)
    second = bootstrap_mean_ci(sample, seed=99, resamples=2000)
    assert first == second
    assert first.statistic == "mean"
    assert first.point == pytest.approx(float(np.mean(sample)))


def test_bootstrap_of_a_constant_sample_has_zero_width():
    interval = bootstrap_median_ci([5.0] * 12, seed=1, resamples=500)
    assert interval.low == 5.0
    assert interval.high == 5.0


def test_bootstrap_rejects_a_non_positive_resample_count():
    with pytest.raises(ValueError):
        bootstrap_median_ci([1.0, 2.0], seed=1, resamples=0)


def test_bootstrap_rejects_an_out_of_range_confidence_level():
    with pytest.raises(ValueError):
        bootstrap_median_ci([1.0, 2.0], seed=1, resamples=10, level=1.0)


# -----------------------------------------------------------------------------
# Outliers are reported, never removed
# -----------------------------------------------------------------------------


def test_outlier_report_flags_the_extreme_point_without_dropping_it():
    sample = [10.0, 10.2, 9.8, 10.1, 9.9, 10.0, 250.0]
    report = outlier_report(sample)

    assert report.indices == (6,)
    assert report.values == (250.0,)
    # The whole point of the module's policy: the sample is untouched.
    assert report.sample_size == len(sample)
    assert report.count == 1
    assert report.fraction == pytest.approx(1 / 7)
    assert not report.degenerate_scale


def test_outlier_report_leaves_the_caller_sample_object_unmodified():
    sample = [10.0, 10.2, 9.8, 10.1, 9.9, 10.0, 250.0]
    before = list(sample)
    outlier_report(sample)
    assert sample == before


def test_outlier_report_finds_nothing_in_a_tight_sample():
    report = outlier_report([10.0, 10.1, 9.9, 10.05, 9.95, 10.02])
    assert report.indices == ()
    assert report.sample_size == 6


def test_outlier_report_masking_resistance():
    # Two large points, not one. A mean-and-sigma rule would let them inflate
    # the scale and hide each other; the robust rule flags both.
    sample = [1.0, 1.1, 0.9, 1.05, 0.95, 1.0, 40.0, 42.0]
    report = outlier_report(sample)
    assert report.indices == (6, 7)
    assert report.sample_size == 8


def test_outlier_report_reports_a_degenerate_scale_rather_than_inventing_one():
    sample = [5.0, 5.0, 5.0, 5.0, 5.0, 9.0]
    report = outlier_report(sample)
    assert report.mad == 0.0
    assert report.degenerate_scale
    assert report.indices == (5,)
    assert report.sample_size == 6


# -----------------------------------------------------------------------------
# Paired comparison: direction, detection and non-detection
# -----------------------------------------------------------------------------


def test_paired_test_detects_a_known_constant_offset():
    # B is uniformly 5.0 faster than A on every query. The query difficulties
    # themselves span three orders of magnitude, which is the situation the
    # pairing exists to handle.
    slow = [1.0, 12.0, 130.0, 1400.0, 17.0, 250.0, 3.0, 88.0, 900.0, 45.0,
            7.0, 610.0, 22.0, 340.0, 5.0, 77.0, 1200.0, 30.0, 190.0, 60.0]
    fast = [value - 5.0 for value in slow]

    result = compare_paired(slow, fast, seed=11, name_a="slow", name_b="fast",
                            resamples=2000)

    assert result.pairs == 20
    assert result.median_difference == pytest.approx(5.0)
    assert result.b_is_faster
    assert result.fraction_b_faster == 1.0
    assert result.rank_biserial == pytest.approx(1.0)
    assert result.p_value < 0.05
    assert result.difference_ci.low <= 5.0 <= result.difference_ci.high


def test_paired_test_reports_no_difference_for_identical_samples():
    sample = [1.0, 12.0, 130.0, 1400.0, 17.0, 250.0, 3.0, 88.0, 900.0, 45.0]
    result = compare_paired(sample, list(sample), seed=5, resamples=1000)

    assert result.median_difference == 0.0
    assert result.median_ratio == pytest.approx(1.0)
    assert result.rank_biserial == 0.0
    assert result.ties == 10
    assert result.p_value == 1.0
    assert result.difference_ci.low == 0.0
    assert result.difference_ci.high == 0.0


def test_paired_test_does_not_detect_zero_mean_noise():
    base = [100.0 + 10.0 * i for i in range(40)]
    noise_a = _deterministic_noise(seed=1001, count=40, scale=2.0)
    noise_b = _deterministic_noise(seed=2002, count=40, scale=2.0)
    a = [b + n for b, n in zip(base, noise_a, strict=True)]
    b = [x + n for x, n in zip(base, noise_b, strict=True)]

    result = compare_paired(a, b, seed=7, resamples=4000)
    assert result.p_value > 0.05


def test_direction_is_reported_consistently_when_a_is_faster():
    fast = [10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0]
    slow = [value * 2.0 for value in fast]

    result = compare_paired(fast, slow, seed=13, name_a="fast", name_b="slow",
                            resamples=2000)

    # A is faster here, so every direction-carrying field must point the other
    # way from the first detection test. A sign error would show up here.
    assert result.median_difference < 0.0
    assert not result.b_is_faster
    assert result.median_ratio == pytest.approx(0.5)
    assert result.speedup_of_b_over_a == pytest.approx(0.5)
    assert result.rank_biserial == pytest.approx(-1.0)
    assert result.fraction_b_faster == 0.0

    # Swapping the arguments must negate the differences and invert the ratio.
    swapped = compare_paired(slow, fast, seed=13, name_a="slow", name_b="fast",
                             resamples=2000)
    assert swapped.median_difference == pytest.approx(-result.median_difference)
    assert swapped.median_ratio == pytest.approx(1.0 / result.median_ratio)
    assert swapped.rank_biserial == pytest.approx(-result.rank_biserial)
    assert swapped.name_a == "slow" and swapped.name_b == "fast"


def test_permutation_p_value_is_identical_for_the_same_seed():
    a = [float(x) for x in range(1, 31)]
    b = [value * 1.10 for value in a]

    first = paired_permutation_test(a, b, seed=42, resamples=3000)
    second = paired_permutation_test(a, b, seed=42, resamples=3000)
    assert first == second


def test_permutation_p_value_with_a_different_seed_is_close():
    a = [float(x) for x in range(1, 31)]
    b = [value * 1.10 for value in a]

    first = paired_permutation_test(a, b, seed=42, resamples=5000)
    second = paired_permutation_test(a, b, seed=43, resamples=5000)
    assert 0.0 < first <= 1.0 and 0.0 < second <= 1.0
    assert first == pytest.approx(second, abs=0.02)


def test_permutation_p_value_is_never_zero():
    # An overwhelming effect still cannot be resolved beyond 1 / (1 + resamples).
    a = [1.0] * 25
    b = [1000.0] * 25
    p = paired_permutation_test(a, b, seed=3, resamples=199)
    assert p == pytest.approx(1.0 / 200.0)
    assert p > 0.0


def test_median_statistic_is_degenerate_on_a_constant_offset():
    # Kept as a regression test because it is the reason the default statistic
    # is the mean: sign-flipping a constant difference never changes the
    # absolute median, so the median-statistic form of the test cannot see an
    # effect that is perfectly consistent. The mean-statistic form sees it
    # immediately. If the default is ever changed back, this fails.
    a = [float(x) for x in range(1, 26)]
    b = [value - 5.0 for value in a]

    with_median = paired_permutation_test(a, b, seed=4, resamples=2000,
                                          statistic="median")
    with_mean = paired_permutation_test(a, b, seed=4, resamples=2000,
                                        statistic="mean")

    assert with_median == 1.0
    assert with_mean < 0.05


def test_permutation_p_value_is_one_when_every_difference_is_zero():
    sample = [3.0, 9.0, 27.0, 81.0]
    assert paired_permutation_test(sample, list(sample), seed=8, resamples=500) == 1.0


def test_wilcoxon_is_available_as_an_alternative_and_agrees_on_a_clear_effect():
    slow = [10.0, 22.0, 35.0, 47.0, 51.0, 68.0, 79.0, 83.0, 95.0, 110.0,
            121.0, 134.0, 148.0, 155.0, 163.0]
    fast = [value * 0.8 for value in slow]

    permutation = compare_paired(slow, fast, seed=21, resamples=4000,
                                 test=TEST_PERMUTATION)
    wilcoxon = compare_paired(slow, fast, seed=21, resamples=4000,
                              test=TEST_WILCOXON)

    assert permutation.test == TEST_PERMUTATION
    assert wilcoxon.test == TEST_WILCOXON
    assert permutation.p_value < 0.05
    assert wilcoxon.p_value < 0.05
    # The two tests differ only in the p-value; the descriptive fields must not
    # depend on which test was asked for.
    assert wilcoxon.median_difference == permutation.median_difference
    assert wilcoxon.rank_biserial == permutation.rank_biserial


def test_wilcoxon_handles_all_identical_measurements_without_warning():
    sample = [4.0, 8.0, 15.0, 16.0, 23.0, 42.0]
    assert wilcoxon_signed_rank_test(sample, list(sample)) == 1.0


def test_median_ratio_is_withheld_when_a_denominator_is_not_positive():
    a = [1.0, 2.0, 3.0, 4.0]
    b = [1.0, 0.0, 3.0, 4.0]
    result = compare_paired(a, b, seed=2, resamples=500)
    assert result.median_ratio is None
    assert result.speedup_of_b_over_a is None
    assert math.isfinite(result.median_difference)


# -----------------------------------------------------------------------------
# Degenerate input is refused, not repaired
# -----------------------------------------------------------------------------


def test_mismatched_lengths_raise_rather_than_truncating():
    a = [1.0, 2.0, 3.0, 4.0]
    b = [1.0, 2.0, 3.0]
    with pytest.raises(ValueError) as excinfo:
        compare_paired(a, b, seed=1, name_a="binary_heap", name_b="lpq")
    message = str(excinfo.value)
    assert "binary_heap" in message and "lpq" in message


def test_mismatched_lengths_raise_in_the_bare_permutation_test_too():
    with pytest.raises(ValueError):
        paired_permutation_test([1.0, 2.0], [1.0], seed=1, resamples=10)


def test_mismatched_lengths_raise_in_the_wilcoxon_wrapper_too():
    with pytest.raises(ValueError):
        wilcoxon_signed_rank_test([1.0, 2.0], [1.0])


def test_empty_input_raises():
    with pytest.raises(ValueError):
        compare_paired([], [], seed=1)


def test_non_finite_measurement_raises():
    with pytest.raises(ValueError):
        compare_paired([1.0, float("inf")], [1.0, 2.0], seed=1)


def test_unknown_test_name_raises():
    with pytest.raises(ValueError):
        compare_paired([1.0, 2.0], [2.0, 3.0], seed=1, test="t-test")


# -----------------------------------------------------------------------------
# Multiple-comparison correction
# -----------------------------------------------------------------------------


def test_holm_raises_p_values_and_preserves_order():
    raw = (0.001, 0.01, 0.02, 0.04)
    adjusted = adjust_p_values(raw, method=CORRECTION_HOLM)

    assert len(adjusted) == len(raw)
    for original, corrected in zip(raw, adjusted, strict=True):
        assert corrected >= original
    assert any(corrected > original for original, corrected in zip(raw, adjusted, strict=True))
    # Monotone in the raw values, so the ranking of the comparisons survives.
    assert list(adjusted) == sorted(adjusted)


def test_benjamini_hochberg_raises_p_values_and_preserves_order():
    raw = (0.001, 0.01, 0.02, 0.04)
    adjusted = adjust_p_values(raw, method=CORRECTION_BENJAMINI_HOCHBERG)

    for original, corrected in zip(raw, adjusted, strict=True):
        assert corrected >= original
    assert list(adjusted) == sorted(adjusted)


def test_holm_is_at_least_as_conservative_as_benjamini_hochberg():
    raw = (0.001, 0.01, 0.02, 0.04, 0.2)
    holm = adjust_p_values(raw, method=CORRECTION_HOLM)
    bh = adjust_p_values(raw, method=CORRECTION_BENJAMINI_HOCHBERG)
    for h, f in zip(holm, bh, strict=True):
        assert h >= f


def test_correction_returns_values_in_input_order_not_sorted_order():
    raw = (0.04, 0.001, 0.02)
    adjusted = adjust_p_values(raw, method=CORRECTION_HOLM)
    # The largest raw value came first, so the largest adjusted value must too.
    assert adjusted[0] == max(adjusted)
    assert adjusted[1] == min(adjusted)


def test_correction_never_exceeds_one():
    adjusted = adjust_p_values((0.5, 0.6, 0.9), method=CORRECTION_HOLM)
    assert all(value <= 1.0 for value in adjusted)


def test_correction_of_an_empty_family_is_empty():
    assert adjust_p_values(()) == ()


def test_correction_rejects_values_outside_the_unit_interval():
    with pytest.raises(ValueError):
        adjust_p_values((0.5, 1.5))


def test_adjust_comparisons_fills_in_adjusted_values_and_keeps_the_raw_ones():
    base = [10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0,
            110.0, 120.0, 130.0, 140.0, 150.0]
    comparisons = [
        compare_paired(base, [v * factor for v in base], seed=seed,
                       name_a="baseline", name_b=f"candidate_{index}",
                       resamples=1000)
        for index, (factor, seed) in enumerate(
            [(0.5, 1), (0.9, 2), (1.0, 3), (1.4, 4)]
        )
    ]

    adjusted = adjust_comparisons(comparisons, method=CORRECTION_HOLM)

    assert len(adjusted) == len(comparisons)
    for before, after in zip(comparisons, adjusted, strict=True):
        assert before.adjusted_p_value is None
        assert after.p_value == before.p_value  # the raw value is preserved
        assert after.adjusted_p_value is not None
        assert after.adjusted_p_value >= after.p_value
        assert after.correction == CORRECTION_HOLM
        assert after.name_b == before.name_b
        assert after.median_difference == before.median_difference


def test_adjust_comparisons_of_an_empty_family_is_empty():
    assert adjust_comparisons([]) == ()
