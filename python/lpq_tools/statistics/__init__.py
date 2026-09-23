"""Statistical analysis of benchmark results (PRD.md section 22, ESD.md section 31).

Two halves, matching the two questions a benchmark asks:

``summary``
    What does one configuration's sample of measurements look like? Robust
    descriptive statistics, bootstrap confidence intervals, and an outlier
    report that flags extreme points without removing any.

``comparison``
    Is the difference between two configurations real, and how large is it?
    Paired tests on identical query sets, effect sizes, and multiple-comparison
    correction for the baseline matrix.

Every randomised procedure here takes an explicit seed and is reproducible from
it. Nothing in this package removes a measurement from a sample.
"""

from lpq_tools.statistics.comparison import (
    CORRECTION_BENJAMINI_HOCHBERG,
    CORRECTION_HOLM,
    TEST_PERMUTATION,
    TEST_WILCOXON,
    PairedComparison,
    adjust_comparisons,
    adjust_p_values,
    compare_paired,
    paired_permutation_test,
    wilcoxon_signed_rank_test,
)
from lpq_tools.statistics.summary import (
    DEFAULT_OUTLIER_THRESHOLD,
    DEFAULT_RESAMPLES,
    MAD_TO_SIGMA,
    ConfidenceInterval,
    OutlierReport,
    SampleSummary,
    bootstrap_mean_ci,
    bootstrap_median_ci,
    bootstrap_statistic,
    outlier_report,
    splitmix64_stream,
    summarise,
)

__all__ = [
    "CORRECTION_BENJAMINI_HOCHBERG",
    "CORRECTION_HOLM",
    "DEFAULT_OUTLIER_THRESHOLD",
    "DEFAULT_RESAMPLES",
    "MAD_TO_SIGMA",
    "TEST_PERMUTATION",
    "TEST_WILCOXON",
    "ConfidenceInterval",
    "OutlierReport",
    "PairedComparison",
    "SampleSummary",
    "adjust_comparisons",
    "adjust_p_values",
    "bootstrap_mean_ci",
    "bootstrap_median_ci",
    "bootstrap_statistic",
    "compare_paired",
    "outlier_report",
    "paired_permutation_test",
    "splitmix64_stream",
    "summarise",
    "wilcoxon_signed_rank_test",
]
