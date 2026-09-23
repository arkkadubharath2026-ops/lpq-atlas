"""Descriptive statistics for a sample of timings -- PRD.md section 22.

PRD.md section 22 requires that we

    Report distributions, not only averages.

and that

    Outliers must not be silently removed.

Both requirements shape this module more than any efficiency concern does.

-------------------------------------------------------------------------------
Why the median and the MAD are first-class, not an afterthought
-------------------------------------------------------------------------------

The development host is a hybrid-core laptop part: a small number of performance
cores alongside a larger number of efficiency cores. Even with a benchmark
thread pinned, the machine around it is not quiet -- frequency changes, thermal
limits and the occasional scheduling artefact all land in the measured sample as
a few very large values. Timing distributions are right-skewed to begin with,
and these events lengthen the right tail further; nothing ever makes a run
finish implausibly early.

A mean and a standard deviation summarise that badly. A single sample an order
of magnitude above the rest can move the mean by more than the effect size we
are trying to measure, and it inflates the standard deviation enough that a real
difference stops being visible. The median and the median absolute deviation are
unaffected by that same sample: they describe the behaviour of a typical run,
which is what a runtime claim is about.

The mean and the standard deviation are still reported, because PRD.md section
22 asks for them and because a large gap between mean and median is itself the
signal that the tail is doing something. They are reported alongside the robust
statistics rather than instead of them.

-------------------------------------------------------------------------------
Why outliers are reported and never dropped
-------------------------------------------------------------------------------

``outlier_report`` identifies points far from the median on a robust scale and
returns their positions. It does not return a filtered sample, and there is no
function in this module that does. That is deliberate: a trimming rule applied
after looking at the results is indistinguishable from choosing the answer, and
PRD.md section 22 requires that any exclusion be documented before the results
are analysed. A report lets an exclusion be argued for in the open; a silent
filter hides that there was ever anything to argue about.

The report is also diagnostic in its own right. A benchmark whose outlier count
jumps between configurations is usually telling us about the measurement
environment rather than about the algorithm, and that is worth seeing.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from lpq_tools.data.queries import SplitMix64

#: Consistency constant relating the MAD to the standard deviation of a normal
#: distribution: for normal data, ``MAD * 1.4826`` estimates sigma. The MAD is
#: stored unscaled in :class:`SampleSummary` -- scaling it would quietly assert
#: normality, which timing samples do not have -- and the constant is exported
#: for callers who want a sigma-comparable number and know why.
MAD_TO_SIGMA = 1.4826

#: Reciprocal of ``MAD_TO_SIGMA``: the constant in Iglewicz and Hoaglin's
#: modified z-score, ``0.6745 * (x - median) / MAD``.
_MODIFIED_Z_CONSTANT = 0.6745

#: Default cut for the modified z-score. 3.5 is the conventional value and is
#: roughly a 3.5-sigma cut for normal data, so it flags genuinely extreme points
#: rather than merely the tail that every timing sample has.
DEFAULT_OUTLIER_THRESHOLD = 3.5

#: Default bootstrap resample count. Large enough that the percentile endpoints
#: of a 95% interval are stable to the precision we report, and small enough to
#: stay well inside a second for the sample sizes a benchmark produces.
DEFAULT_RESAMPLES = 10_000

_MASK64 = (1 << 64) - 1
_GOLDEN = 0x9E3779B97F4A7C15


@dataclass(frozen=True)
class SampleSummary:
    """Descriptive statistics for one sample of measurements.

    Every field is computed from the sample exactly as given: nothing is
    trimmed, winsorised or otherwise filtered on the way in.
    """

    count: int
    minimum: float
    maximum: float
    mean: float
    stddev: float
    median: float
    #: Median absolute deviation, ``median(|x - median(x)|)``, unscaled. See
    #: ``MAD_TO_SIGMA`` for why it is not normalised here.
    mad: float
    p25: float
    p75: float
    p95: float
    p99: float
    #: ``p75 - p25``. Reported next to the MAD because the two disagree in an
    #: informative way: the IQR widens when the bulk of the distribution is
    #: skewed, the MAD does not.
    iqr: float


@dataclass(frozen=True)
class ConfidenceInterval:
    """A bootstrap percentile interval, with everything needed to reproduce it.

    The seed and the resample count are stored rather than left behind in the
    caller's script because an interval is a reported number: ESD.md section 23
    requires a result record to carry what produced it, and an interval whose
    seed has been lost cannot be checked.
    """

    point: float
    low: float
    high: float
    level: float
    resamples: int
    seed: int
    #: The statistic the interval is for, e.g. ``"median"`` or ``"mean"``.
    statistic: str
    method: str = "bootstrap-percentile"


@dataclass(frozen=True)
class OutlierReport:
    """Which points are extreme -- and, pointedly, the sample they came from.

    ``sample_size`` is the size of the sample that was examined and is also the
    size of the sample the caller should carry on using. Nothing here removes a
    point; see the module docstring.
    """

    sample_size: int
    median: float
    mad: float
    threshold: float
    #: Positions of the flagged points within the input sequence, ascending.
    indices: tuple[int, ...]
    #: The flagged values themselves, in the same order as ``indices``.
    values: tuple[float, ...]
    #: True when the MAD is zero, which happens when more than half the sample
    #: is a single repeated value. There is then no robust scale to measure
    #: distance in, so anything unequal to the median is flagged and this flag
    #: warns the caller that the threshold did not really apply.
    degenerate_scale: bool = False

    @property
    def count(self) -> int:
        return len(self.indices)

    @property
    def fraction(self) -> float:
        if self.sample_size == 0:
            return 0.0
        return len(self.indices) / self.sample_size


def _as_array(values: object, *, name: str = "values") -> np.ndarray:
    """Converts a sequence of measurements to a float array, or explains why not."""
    array = np.asarray(values, dtype=np.float64).reshape(-1)
    if array.size == 0:
        raise ValueError(f"{name} is empty; there is nothing to summarise")
    if not bool(np.all(np.isfinite(array))):
        raise ValueError(
            f"{name} contains a non-finite value; a NaN or an infinity in a "
            "timing sample is a measurement failure and must be investigated "
            "rather than summarised"
        )
    return array


def summarise(values: object) -> SampleSummary:
    """Computes descriptive statistics for a sample.

    Raises ``ValueError`` on an empty sample and on a sample containing NaN or
    infinity. Silently producing a NaN summary would let a broken measurement
    propagate all the way into a table.

    The example below is synthetic, not a measurement from this project:

    >>> summary = summarise([1.0, 2.0, 3.0, 4.0, 100.0])
    >>> summary.median
    3.0
    >>> summary.count
    5
    """
    array = _as_array(values)
    median = float(np.median(array))
    mad = float(np.median(np.abs(array - median)))
    p25, p75, p95, p99 = (float(x) for x in np.percentile(array, [25, 75, 95, 99]))
    # ddof=1 when n > 1: the sample estimates the behaviour of the
    # configuration, not the whole population of runs. For n == 1 there is no
    # spread to estimate and numpy would emit a warning, so it is reported as
    # zero.
    stddev = float(np.std(array, ddof=1)) if array.size > 1 else 0.0
    return SampleSummary(
        count=int(array.size),
        minimum=float(np.min(array)),
        maximum=float(np.max(array)),
        mean=float(np.mean(array)),
        stddev=stddev,
        median=median,
        mad=mad,
        p25=p25,
        p75=p75,
        p95=p95,
        p99=p99,
        iqr=p75 - p25,
    )


def splitmix64_stream(seed: int, count: int) -> np.ndarray:
    """Generates ``count`` SplitMix64 outputs as a ``uint64`` array.

    This is a vectorised restatement of
    :class:`lpq_tools.data.queries.SplitMix64`, and is tested against it value
    for value.

    Why not ``numpy.random.Generator``? Because numpy documents that
    ``Generator``'s stream may change between releases -- only the legacy
    ``RandomState`` stream is frozen -- and a confidence interval that shifts
    after a routine dependency upgrade is not reproducible in the sense PRD.md
    section 23 requires. SplitMix64 is defined by these few lines and by nothing
    else, so a resampling procedure seeded with it produces the same interval on
    any machine that has the repository. This is the one place where the
    project's preference for pinned library code over hand-written numerics is
    inverted, because here the determinism guarantee is the whole point.

    The generator's state advances by a fixed additive constant, so the state
    sequence has a closed form and only the mixing function has to be applied
    elementwise. The loop that ``SplitMix64`` runs in Python therefore becomes a
    few numpy operations, which is what makes ten thousand resamples of a large
    sample practical.
    """
    if count < 0:
        raise ValueError("count must not be negative")
    if count == 0:
        return np.zeros(0, dtype=np.uint64)
    steps = np.arange(1, count + 1, dtype=np.uint64)
    state = np.uint64(seed & _MASK64) + steps * np.uint64(_GOLDEN)
    z = state
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    return z ^ (z >> np.uint64(31))


def _unit_stream(seed: int, count: int) -> np.ndarray:
    """Deterministic doubles on ``[0, 1)``, from the top 53 bits of each word."""
    words = splitmix64_stream(seed, count)
    return (words >> np.uint64(11)).astype(np.float64) * (1.0 / float(1 << 53))


def _resample_indices(seed: int, resamples: int, n: int) -> np.ndarray:
    """Draws bootstrap indices uniformly on ``[0, n)``, deterministically.

    Scaling a ``[0, 1)`` double leaves no residue bias worth naming -- the
    departure from uniformity is of order ``n / 2**53`` -- and, unlike rejection
    sampling, it is branch-free, so it vectorises and the number of random words
    consumed does not depend on the data.
    """
    unit = _unit_stream(seed, resamples * n)
    indices = (unit * float(n)).astype(np.int64)
    # Guards against a value reaching ``n`` through rounding at the top of the
    # range; cheaper than reasoning about the floating-point edge case.
    np.clip(indices, 0, n - 1, out=indices)
    return indices.reshape((resamples, n))


def bootstrap_statistic(
    values: object,
    *,
    seed: int,
    statistic: str = "median",
    resamples: int = DEFAULT_RESAMPLES,
    level: float = 0.95,
) -> ConfidenceInterval:
    """Bootstrap percentile interval for the median or the mean of a sample.

    -------------------------------------------------------------------------
    Why a bootstrap rather than a normal-theory interval
    -------------------------------------------------------------------------

    For the mean, the textbook interval ``mean +/- t * s / sqrt(n)`` leans on
    the central limit theorem and on the sample standard deviation being a
    usable scale estimate. Neither is comfortable here: benchmark samples are
    small (a manifest's repetition count is in the tens), right-skewed, and
    their standard deviation is exactly the quantity a scheduling outlier
    destroys.

    For the median it is worse. The asymptotic standard error of a median
    involves the density of the underlying distribution at the median, which we
    do not know and cannot estimate well from a few dozen points. There is no
    honest normal-theory interval for the median of a timing sample.

    The bootstrap sidesteps both problems: it resamples the observed sample with
    replacement, recomputes the statistic on each resample, and reads the
    interval off the resulting distribution of replicates. It assumes only that
    the sample is representative of what it was drawn from, which is the
    assumption already made in reporting the statistic at all. Because the
    resampling is driven by an explicit seed, the interval is exactly
    reproducible.

    Percentile endpoints are used rather than BCa. BCa corrects for bias and
    skew and would be a little tighter, but its acceleration term is a jackknife
    over the sample, which is itself unstable at these sample sizes; the
    percentile interval is the more conservative and the more predictable of the
    two.
    """
    array = _as_array(values)
    if statistic not in ("median", "mean"):
        raise ValueError(f"unsupported statistic {statistic!r}; use 'median' or 'mean'")
    if resamples < 1:
        raise ValueError("resamples must be at least 1")
    if not 0.0 < level < 1.0:
        raise ValueError("level must lie strictly between 0 and 1")

    reducer = np.median if statistic == "median" else np.mean
    point = float(reducer(array))

    n = int(array.size)
    indices = _resample_indices(seed, int(resamples), n)
    replicates = reducer(array[indices], axis=1)

    tail = (1.0 - level) / 2.0
    low, high = np.percentile(replicates, [100.0 * tail, 100.0 * (1.0 - tail)])
    return ConfidenceInterval(
        point=point,
        low=float(low),
        high=float(high),
        level=float(level),
        resamples=int(resamples),
        seed=int(seed),
        statistic=statistic,
    )


def bootstrap_median_ci(
    values: object,
    *,
    seed: int,
    resamples: int = DEFAULT_RESAMPLES,
    level: float = 0.95,
) -> ConfidenceInterval:
    """Bootstrap percentile interval for the median; see ``bootstrap_statistic``."""
    return bootstrap_statistic(
        values, seed=seed, statistic="median", resamples=resamples, level=level
    )


def bootstrap_mean_ci(
    values: object,
    *,
    seed: int,
    resamples: int = DEFAULT_RESAMPLES,
    level: float = 0.95,
) -> ConfidenceInterval:
    """Bootstrap percentile interval for the mean; see ``bootstrap_statistic``."""
    return bootstrap_statistic(
        values, seed=seed, statistic="mean", resamples=resamples, level=level
    )


def outlier_report(
    values: object,
    *,
    threshold: float = DEFAULT_OUTLIER_THRESHOLD,
) -> OutlierReport:
    """Identifies extreme points on a robust scale, without removing any.

    The rule is the modified z-score of Iglewicz and Hoaglin,
    ``0.6745 * (x - median) / MAD``, flagged above ``threshold``. Median and MAD
    are used rather than mean and standard deviation for the reason a robust
    rule is wanted in the first place: a mean-and-sigma cut is dragged towards
    the very points it is meant to find, so one large outlier can widen the
    window enough to conceal itself and a second one -- the masking effect.

    The returned report names the flagged positions and leaves the sample
    intact; ``sample_size`` is the unchanged input size. This module offers no
    way to drop the flagged points, because PRD.md section 22 forbids silent
    removal and requires that any exclusion be argued for and documented before
    the analysis. Flagging is evidence for that argument, not a substitute for
    it.

    The example below is synthetic, not a measurement from this project:

    >>> report = outlier_report([1.0, 1.1, 0.9, 1.05, 50.0])
    >>> report.indices
    (4,)
    >>> report.sample_size
    5
    """
    array = _as_array(values)
    if threshold <= 0.0:
        raise ValueError("threshold must be positive")

    median = float(np.median(array))
    deviations = np.abs(array - median)
    mad = float(np.median(deviations))

    if mad == 0.0:
        # More than half the sample equals the median, so there is no robust
        # scale to divide by. Rather than invent one, flag whatever differs and
        # say so through ``degenerate_scale``.
        flagged = np.flatnonzero(array != median)
        degenerate = True
    else:
        scores = _MODIFIED_Z_CONSTANT * deviations / mad
        flagged = np.flatnonzero(scores > threshold)
        degenerate = False

    return OutlierReport(
        sample_size=int(array.size),
        median=median,
        mad=mad,
        threshold=float(threshold),
        indices=tuple(int(i) for i in flagged),
        values=tuple(float(array[i]) for i in flagged),
        degenerate_scale=degenerate,
    )


__all__ = [
    "DEFAULT_OUTLIER_THRESHOLD",
    "DEFAULT_RESAMPLES",
    "MAD_TO_SIGMA",
    "ConfidenceInterval",
    "OutlierReport",
    "SampleSummary",
    "SplitMix64",
    "bootstrap_mean_ci",
    "bootstrap_median_ci",
    "bootstrap_statistic",
    "outlier_report",
    "splitmix64_stream",
    "summarise",
]
