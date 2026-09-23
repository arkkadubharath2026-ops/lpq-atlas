"""Paired comparison of two queues or algorithms -- PRD.md section 22.

PRD.md section 22 asks for four things when a performance claim is made:

    - Prefer paired comparisons on identical query sets.
    - Use appropriate significance tests when comparing algorithms.
    - Report effect sizes, not only p-values.
    - Report distributions, not only averages.

This module implements the first three. The fourth lives in
``lpq_tools.statistics.summary``.

-------------------------------------------------------------------------------
Why the comparison must be paired
-------------------------------------------------------------------------------

Per-query difficulty in a shortest-path benchmark varies by orders of magnitude.
On a road network, a query between two neighbouring suburbs settles a thousand
times faster than a query across the whole country, and the manifest deliberately
contains both -- PRD.md section 18 forbids cherry-picking the easy ones.

That spread is between queries. An unpaired test treats it as noise, so the
noise term it forms is dominated by how hard the queries are rather than by
which queue ran them. A twenty per cent difference between two queues is then
invisible underneath a thousandfold difference between two queries, and the test
reports nothing when there is plainly something. Pairing removes that variance
entirely: because both queues ran the *same* query, the per-query difference is
the quantity of interest with the difficulty divided out, and what is left to
call noise is only the measurement jitter. Nearly all of the statistical power
in this benchmark comes from that one structural choice.

The correctness condition for pairing is that element ``i`` of each sample is
the same query under both configurations. That is why a length mismatch raises
here rather than truncating: truncation would silently pair query ``i`` of one
configuration against a different query of the other, and the result would look
entirely normal.

-------------------------------------------------------------------------------
Direction
-------------------------------------------------------------------------------

A sign error in a paired comparison inverts the conclusion without producing
anything that looks wrong, so the convention is fixed and stated everywhere it
could matter:

    differences[i] = a[i] - b[i]        ratios[i] = a[i] / b[i]

With timings as the input, a positive ``median_difference`` means A took longer,
which means **B is the faster configuration**. ``speedup_of_b_over_a`` is the
median ratio, and is greater than one exactly when B is faster.
:class:`PairedComparison` carries ``name_a`` and ``name_b`` so a stored result
says which is which without depending on argument order being remembered.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import stats as scipy_stats
from statsmodels.stats.multitest import multipletests

from lpq_tools.statistics.summary import (
    DEFAULT_RESAMPLES,
    ConfidenceInterval,
    _resample_indices,
    _unit_stream,
)

#: Permutation tests and Wilcoxon's test are both distribution-free; the choice
#: between them is described in ``paired_permutation_test``.
TEST_PERMUTATION = "paired-permutation"
TEST_WILCOXON = "wilcoxon-signed-rank"

#: Correction methods, named as this module's callers name them. They map onto
#: statsmodels' own identifiers in ``adjust_p_values``.
CORRECTION_HOLM = "holm"
CORRECTION_BENJAMINI_HOCHBERG = "benjamini-hochberg"

_STATSMODELS_METHOD = {
    CORRECTION_HOLM: "holm",
    CORRECTION_BENJAMINI_HOCHBERG: "fdr_bh",
}


@dataclass(frozen=True)
class PairedComparison:
    """The result of comparing configuration A against configuration B.

    Every field follows the direction convention in the module docstring:
    differences and ratios are A relative to B.
    """

    name_a: str
    name_b: str
    #: Number of matched queries. Both samples had exactly this length.
    pairs: int

    #: Median of ``a[i] - b[i]``. Positive means A is slower than B.
    median_difference: float
    #: Median of ``a[i] / b[i]``, or ``None`` when some ``b[i]`` is not
    #: strictly positive and the ratio is therefore undefined.
    median_ratio: float | None
    #: Bootstrap percentile interval for ``median_difference``.
    difference_ci: ConfidenceInterval

    p_value: float
    #: ``TEST_PERMUTATION`` or ``TEST_WILCOXON``.
    test: str
    alternative: str

    #: Median ratio expressed as a speedup: greater than one when B is faster
    #: than A. ``None`` whenever ``median_ratio`` is ``None``.
    speedup_of_b_over_a: float | None
    #: Matched-pairs rank-biserial correlation, in ``[-1, 1]``. Positive means
    #: A is slower. It is the rank-based companion to the median ratio: it says
    #: how consistently one configuration wins, where the ratio says by how
    #: much, and it is unaffected by how heavy the tail of the difference
    #: distribution is.
    rank_biserial: float
    #: Fraction of queries on which B was strictly faster than A. Reported
    #: because a median ratio below one accompanied by, say, half the queries
    #: going the other way is a different finding from a consistent win.
    fraction_b_faster: float
    #: Queries where the two configurations timed identically; excluded from
    #: the rank statistic, as Wilcoxon's test excludes them.
    ties: int

    #: Adjusted p-value once this comparison has been placed in a family; see
    #: ``adjust_comparisons``. ``None`` until then, and never silently equal to
    #: the raw value.
    adjusted_p_value: float | None = None
    correction: str | None = None

    @property
    def b_is_faster(self) -> bool:
        """True when the median difference points at B, ignoring significance."""
        return self.median_difference > 0.0


def _paired_arrays(
    a: object,
    b: object,
    *,
    name_a: str,
    name_b: str,
) -> tuple[np.ndarray, np.ndarray]:
    """Validates the two samples and returns them as float arrays.

    Raises rather than repairing anything. A mismatched length is the dangerous
    case: see the module docstring on why truncation would compare different
    queries.
    """
    array_a = np.asarray(a, dtype=np.float64).reshape(-1)
    array_b = np.asarray(b, dtype=np.float64).reshape(-1)

    if array_a.size != array_b.size:
        raise ValueError(
            f"paired comparison needs one measurement per query from each "
            f"configuration, but {name_a} has {array_a.size} and {name_b} has "
            f"{array_b.size}; the shorter sample is not truncated because "
            f"element i must be the same query in both"
        )
    if array_a.size == 0:
        raise ValueError("paired comparison needs at least one pair; both samples are empty")
    if not bool(np.all(np.isfinite(array_a))) or not bool(np.all(np.isfinite(array_b))):
        raise ValueError(
            "a paired sample contains a non-finite value; a NaN or an infinity "
            "is a measurement failure and must be investigated rather than "
            "tested"
        )
    return array_a, array_b


def paired_permutation_test(
    a: object,
    b: object,
    *,
    seed: int,
    resamples: int = DEFAULT_RESAMPLES,
    alternative: str = "two-sided",
    statistic: str = "mean",
    name_a: str = "A",
    name_b: str = "B",
) -> float:
    """Sign-flipping permutation test on the per-query differences.

    -------------------------------------------------------------------------
    What is being tested, and why this test
    -------------------------------------------------------------------------

    Under the null hypothesis that the two configurations are exchangeable on
    each query, the sign of each per-query difference is arbitrary: had the
    labels been swapped for that query, the difference would have been negated
    and everything else would look the same. So the null distribution of the
    test statistic is generated by independently negating each difference with
    probability one half, and the p-value is the share of those relabellings
    whose statistic is at least as extreme as the observed one.

    This assumes nothing about the shape of the difference distribution. That
    matters here: a paired t-test assumes the differences are approximately
    normal, and differences between two timing samples are right-skewed and
    occasionally carry a scheduling outlier, which is exactly the situation in
    which the t-test's tail probabilities stop being trustworthy.

    -------------------------------------------------------------------------
    Why the statistic is the mean difference and not the median
    -------------------------------------------------------------------------

    Everything else in this package leads with the median, so the default here
    deserves an explanation. Under sign-flipping, the median is close to
    useless as a test statistic: negating a difference moves it to the other
    side of zero without changing its magnitude, so when the differences are
    all of similar size the median of any relabelling has essentially the same
    absolute value as the observed one. In the limiting case of a perfectly
    constant offset -- every query slower by the same amount -- the flipped
    median is the offset itself for every one of the relabellings, the observed
    value is never extreme, and the test returns a p-value of 1.0 against an
    effect that is as real and as consistent as an effect can be. The mean, by
    contrast, moves by twice the flipped value each time, so its null
    distribution actually spreads and the test has power.

    This is a statement about the test statistic only. The reported effect is
    still the median difference, the interval around it is still a median
    interval, and nothing is trimmed anywhere. What it does mean is that the
    permutation p-value is sensitive to a very large outlying difference in a
    way the reported median is not, which is one more reason to cross-check a
    marginal result with ``wilcoxon_signed_rank_test``, whose statistic is
    ranked and therefore bounded.

    ``statistic="median"`` remains available for the case where the median is
    genuinely the quantity in question and the differences are known to be
    widely spread; the degeneracy above applies to it.

    -------------------------------------------------------------------------
    The p-value estimator
    -------------------------------------------------------------------------

    The observed arrangement is counted as one of its own relabellings, giving
    ``(1 + extreme) / (1 + resamples)``. This never returns zero, which matters
    because a zero p-value is not a statement the procedure can support -- with
    ``resamples`` draws the resolution is bounded by ``1 / (1 + resamples)``,
    and reporting zero would overstate it.

    ``seed`` is required and not defaulted, so that no result can be produced by
    an unrecorded stream, and the same seed always yields the same p-value.

    When every difference is zero the two configurations are identical query by
    query, no relabelling changes anything, and the p-value is 1.0 by
    construction rather than by special case.
    """
    array_a, array_b = _paired_arrays(a, b, name_a=name_a, name_b=name_b)
    if alternative not in ("two-sided", "less", "greater"):
        raise ValueError(
            f"unsupported alternative {alternative!r}; use 'two-sided', 'less' or 'greater'"
        )
    if statistic not in ("median", "mean"):
        raise ValueError(f"unsupported statistic {statistic!r}; use 'median' or 'mean'")
    if resamples < 1:
        raise ValueError("resamples must be at least 1")

    differences = array_a - array_b
    reducer = np.median if statistic == "median" else np.mean
    observed = float(reducer(differences))

    n = int(differences.size)
    # One uniform draw per (resample, pair), thresholded at one half to give an
    # independent fair sign per pair. Drawing a fresh double per sign rather
    # than slicing bits out of one word keeps the mapping from stream position
    # to sign trivial, which is what makes the p-value auditable from the seed.
    draws = _unit_stream(seed, int(resamples) * n).reshape((int(resamples), n))
    signs = np.where(draws < 0.5, 1.0, -1.0)
    replicates = reducer(signs * differences, axis=1)

    if alternative == "two-sided":
        extreme = int(np.count_nonzero(np.abs(replicates) >= abs(observed)))
    elif alternative == "greater":
        extreme = int(np.count_nonzero(replicates >= observed))
    else:
        extreme = int(np.count_nonzero(replicates <= observed))

    return (1.0 + extreme) / (1.0 + float(resamples))


def wilcoxon_signed_rank_test(
    a: object,
    b: object,
    *,
    alternative: str = "two-sided",
    name_a: str = "A",
    name_b: str = "B",
) -> float:
    """Wilcoxon signed-rank test on the per-query differences, via scipy.

    -------------------------------------------------------------------------
    When to prefer this over the permutation test
    -------------------------------------------------------------------------

    Both tests are distribution-free and both use the pairing. They differ in
    what they are sensitive to.

    Wilcoxon's test replaces the magnitudes by their ranks before summing them,
    so a single enormous difference contributes no more than the next largest
    one. That makes it the more robust of the two when the measurement
    environment is noisy, and its exact null distribution is combinatorial, so
    for small query sets it gives an exact p-value with no resampling and no
    seed at all. Its cost is that it discards magnitude: it answers "does one
    configuration win more often, and by larger ranks" rather than anything
    about how much time was saved. It also assumes the difference distribution
    is symmetric about its centre for the test to be about the median.

    The permutation test keeps the magnitudes, so it is sensitive to an effect
    that is small on most queries and large on a few -- a plausible shape for a
    queue that helps only on long searches, and one a rank test would blunt. It
    is the default for that reason. Wilcoxon's test is the right choice as a
    cross-check, since agreement between a rank test and a magnitude test is
    reassuring and disagreement says the conclusion rests on a handful of large
    differences and should be looked at, and as the primary test when the query
    set is small enough that an exact p-value is worth more than magnitude
    sensitivity.

    When all differences are zero, scipy's test is undefined and would divide by
    zero; that case is answered here with 1.0, which is the correct p-value for
    two configurations that produced identical measurements on every query.
    """
    array_a, array_b = _paired_arrays(a, b, name_a=name_a, name_b=name_b)
    differences = array_a - array_b
    if not np.any(differences != 0.0):
        return 1.0
    result = scipy_stats.wilcoxon(differences, alternative=alternative)
    return float(result.pvalue)


def _rank_biserial(differences: np.ndarray) -> float:
    """Matched-pairs rank-biserial correlation of the per-query differences.

    ``(R+ - R-) / (R+ + R-)`` over the signed ranks of the non-zero
    differences, which is the effect size that belongs with Wilcoxon's
    statistic. It is +1 when A is slower on every query, -1 when B is, and 0
    when the wins and their ranks balance. Ties are dropped from the ranking,
    as Wilcoxon's test drops them, but they remain in ``PairedComparison.ties``
    so that a comparison dominated by identical timings is visible.
    """
    non_zero = differences[differences != 0.0]
    if non_zero.size == 0:
        return 0.0
    ranks = scipy_stats.rankdata(np.abs(non_zero))
    positive = float(np.sum(ranks[non_zero > 0.0]))
    negative = float(np.sum(ranks[non_zero < 0.0]))
    total = positive + negative
    if total == 0.0:
        return 0.0
    return (positive - negative) / total


def compare_paired(
    a: object,
    b: object,
    *,
    seed: int,
    name_a: str = "A",
    name_b: str = "B",
    test: str = TEST_PERMUTATION,
    alternative: str = "two-sided",
    resamples: int = DEFAULT_RESAMPLES,
    level: float = 0.95,
) -> PairedComparison:
    """Compares two configurations measured on the same query set.

    ``a`` and ``b`` must be aligned: element ``i`` of each is the same query.
    Differences and ratios are A relative to B, so a positive median difference
    and a ``speedup_of_b_over_a`` above one both mean B is the faster of the
    two; see the module docstring.

    ``test`` selects ``TEST_PERMUTATION`` (the default, seeded and
    reproducible) or ``TEST_WILCOXON``; ``wilcoxon_signed_rank_test`` explains
    when each is appropriate. The bootstrap interval for the median difference
    is computed either way, and resamples pairs rather than values so that the
    pairing is preserved inside the bootstrap as well.

    Degenerate inputs are refused rather than guessed at: unequal lengths and
    empty samples both raise ``ValueError``. Two identical samples are a
    legitimate input, not a degenerate one -- they yield a zero median
    difference, a zero-width interval, an effect size of zero and a p-value of
    1.0.
    """
    array_a, array_b = _paired_arrays(a, b, name_a=name_a, name_b=name_b)
    if test not in (TEST_PERMUTATION, TEST_WILCOXON):
        raise ValueError(
            f"unsupported test {test!r}; use TEST_PERMUTATION or TEST_WILCOXON"
        )

    differences = array_a - array_b
    pairs = int(differences.size)
    median_difference = float(np.median(differences))

    if bool(np.all(array_b > 0.0)):
        median_ratio: float | None = float(np.median(array_a / array_b))
    else:
        # A zero or negative denominator means the measurement itself is
        # suspect. Reporting ``None`` keeps the rest of the comparison usable
        # while refusing to invent a ratio.
        median_ratio = None

    difference_ci = _bootstrap_paired_median(
        differences, seed=seed, resamples=resamples, level=level
    )

    if test == TEST_PERMUTATION:
        p_value = paired_permutation_test(
            array_a,
            array_b,
            seed=seed,
            resamples=resamples,
            alternative=alternative,
            name_a=name_a,
            name_b=name_b,
        )
    else:
        p_value = wilcoxon_signed_rank_test(
            array_a, array_b, alternative=alternative, name_a=name_a, name_b=name_b
        )

    ties = int(np.count_nonzero(differences == 0.0))
    fraction_b_faster = float(np.count_nonzero(differences > 0.0)) / float(pairs)

    return PairedComparison(
        name_a=name_a,
        name_b=name_b,
        pairs=pairs,
        median_difference=median_difference,
        median_ratio=median_ratio,
        difference_ci=difference_ci,
        p_value=float(p_value),
        test=test,
        alternative=alternative,
        speedup_of_b_over_a=median_ratio,
        rank_biserial=_rank_biserial(differences),
        fraction_b_faster=fraction_b_faster,
        ties=ties,
    )


def _bootstrap_paired_median(
    differences: np.ndarray,
    *,
    seed: int,
    resamples: int,
    level: float,
) -> ConfidenceInterval:
    """Bootstrap percentile interval for the median of the paired differences.

    The resampling unit is the pair, not the individual measurement. Resampling
    the two configurations independently would destroy the pairing that the
    comparison is built on and would reintroduce the between-query variance the
    module docstring explains away.
    """
    if resamples < 1:
        raise ValueError("resamples must be at least 1")
    if not 0.0 < level < 1.0:
        raise ValueError("level must lie strictly between 0 and 1")

    point = float(np.median(differences))
    n = int(differences.size)
    # A distinct seed derivation keeps the bootstrap's stream from coinciding
    # with the permutation test's when both are given the same seed; reusing
    # one stream for two procedures would correlate the interval with the
    # p-value for no reason.
    indices = _resample_indices(seed ^ 0x5D_EE_CE_1B, int(resamples), n)
    replicates = np.median(differences[indices], axis=1)

    tail = (1.0 - level) / 2.0
    low, high = np.percentile(replicates, [100.0 * tail, 100.0 * (1.0 - tail)])
    return ConfidenceInterval(
        point=point,
        low=float(low),
        high=float(high),
        level=float(level),
        resamples=int(resamples),
        seed=int(seed),
        statistic="paired-median-difference",
    )


def adjust_p_values(
    p_values: object,
    *,
    method: str = CORRECTION_HOLM,
    alpha: float = 0.05,
) -> tuple[float, ...]:
    """Corrects a family of p-values for multiple comparison.

    -------------------------------------------------------------------------
    Why this is needed here specifically
    -------------------------------------------------------------------------

    PRD.md section 19's baseline matrix has ten rows: two algorithms across
    five queues. Comparing the LPQ against every classical queue on both
    algorithms is eight tests, and comparing every pair in the matrix is
    forty-five. At an uncorrected five per cent threshold, roughly two of those
    forty-five will look significant on measurements with no real difference in
    them at all. A claim picked out of that matrix after the fact is therefore
    not supported by its own p-value, and the correction is what restores the
    meaning of the threshold.

    Holm-Bonferroni (the default) controls the family-wise error rate: the
    probability of *any* false positive in the family. It is the right choice
    for a headline claim, where one wrong entry discredits the table. It is
    uniformly more powerful than plain Bonferroni and makes no assumption about
    dependence between the tests, which matters because comparisons sharing a
    baseline are correlated by construction.

    Benjamini-Hochberg controls the false discovery rate: the expected share of
    the rejected hypotheses that are false. It is the right choice for
    exploratory sweeps -- an ablation grid, say -- where a few false leads are
    acceptable and the loss of power from family-wise control is not.

    Both are applied through statsmodels rather than reimplemented. They are
    closed-form, deterministic procedures with no seed and no sampling, so
    there is no reproducibility argument for hand-rolling them.

    The returned values are in the same order as the input, never re-sorted --
    a caller matching adjusted values back to comparisons by position must not
    have to know the procedure sorts internally. Adjusted values are clipped to
    1.0 by both procedures and are never smaller than the raw ones.
    """
    array = np.asarray(p_values, dtype=np.float64).reshape(-1)
    if array.size == 0:
        return ()
    if not bool(np.all(np.isfinite(array))) or bool(np.any(array < 0.0)) or bool(np.any(array > 1.0)):
        raise ValueError("p-values must be finite and lie within [0, 1]")
    if method not in _STATSMODELS_METHOD:
        raise ValueError(
            f"unsupported correction {method!r}; use CORRECTION_HOLM or "
            "CORRECTION_BENJAMINI_HOCHBERG"
        )
    adjusted = multipletests(array, alpha=alpha, method=_STATSMODELS_METHOD[method])[1]
    return tuple(float(value) for value in adjusted)


def adjust_comparisons(
    comparisons: object,
    *,
    method: str = CORRECTION_HOLM,
    alpha: float = 0.05,
) -> tuple[PairedComparison, ...]:
    """Places comparisons in one family and fills in their adjusted p-values.

    Returns new :class:`PairedComparison` values in the input order, with
    ``adjusted_p_value`` and ``correction`` set. The raw ``p_value`` is left
    untouched alongside them: a reader should be able to see both, and a stored
    result that quietly held only the adjusted number could not be re-corrected
    if the family later changed.
    """
    items = tuple(comparisons)  # type: ignore[call-overload]
    if not items:
        return ()
    adjusted = adjust_p_values(
        [item.p_value for item in items], method=method, alpha=alpha
    )
    return tuple(
        PairedComparison(
            name_a=item.name_a,
            name_b=item.name_b,
            pairs=item.pairs,
            median_difference=item.median_difference,
            median_ratio=item.median_ratio,
            difference_ci=item.difference_ci,
            p_value=item.p_value,
            test=item.test,
            alternative=item.alternative,
            speedup_of_b_over_a=item.speedup_of_b_over_a,
            rank_biserial=item.rank_biserial,
            fraction_b_faster=item.fraction_b_faster,
            ties=item.ties,
            adjusted_p_value=value,
            correction=method,
        )
        for item, value in zip(items, adjusted, strict=True)
    )


__all__ = [
    "CORRECTION_BENJAMINI_HOCHBERG",
    "CORRECTION_HOLM",
    "TEST_PERMUTATION",
    "TEST_WILCOXON",
    "PairedComparison",
    "adjust_comparisons",
    "adjust_p_values",
    "compare_paired",
    "paired_permutation_test",
    "wilcoxon_signed_rank_test",
]
