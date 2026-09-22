// =============================================================================
// Robust summary statistics for benchmark timings (PRD.md section 22).
//
// WHY MEDIAN AND MAD RATHER THAN MEAN AND STANDARD DEVIATION
//
// The mean and standard deviation are not robust: a single outlier moves both,
// and the standard deviation is moved roughly in proportion to the square of
// the deviation. Benchmark timings on this hardware produce such outliers
// routinely and for reasons unrelated to the code -- the development host is a
// hybrid Intel i5-1235U whose scheduler migrates threads between performance
// and efficiency cores, and which throttles under sustained load. A run that
// happens to be migrated mid-measurement is several times slower than its
// neighbours.
//
// The median tolerates up to half the samples being arbitrarily corrupted; the
// mean tolerates none. The median absolute deviation is the matching robust
// dispersion measure: the median of the absolute deviations from the median.
//
// Both are reported. The mean and standard deviation are ALSO recorded, because
// a large gap between mean and median is itself the diagnostic that says
// outliers were present -- discarding them would hide the very evidence that
// the measurement was disturbed. PRD.md section 22 forbids silently removing
// outliers, so nothing here filters: it summarises robustly and reports the
// full picture, and the raw per-repetition samples are retained so any figure
// can be traced back to actual measurements.
//
// MAD is reported both raw and scaled. Scaling by 1.4826 makes it a consistent
// estimator of the standard deviation for normally distributed data, which is
// what makes it comparable against the reported stddev; the raw value is kept
// because timing distributions are right-skewed rather than normal and the scaling
// assumption does not hold.
// =============================================================================
#ifndef LPQ_METRICS_STATISTICS_HPP
#define LPQ_METRICS_STATISTICS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lpq {

/// Robust and classical summary of a sample of measurements.
struct SampleSummary {
    std::size_t count = 0;

    // --- Robust (primary) -----------------------------------------------------
    double median = 0.0;
    /// Median absolute deviation from the median.
    double mad = 0.0;
    /// MAD * 1.4826, comparable to a standard deviation under normality.
    double mad_scaled = 0.0;

    // --- Classical (reported for comparison, not for decisions) ---------------
    double mean   = 0.0;
    double stddev = 0.0;

    // --- Distribution ---------------------------------------------------------
    double min = 0.0;
    double max = 0.0;
    double p25 = 0.0;
    double p75 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;

    /// Interquartile range.
    [[nodiscard]] double iqr() const noexcept { return p75 - p25; }

    /// Relative dispersion: MAD divided by the median.
    ///
    /// The robust analogue of the coefficient of variation. The benchmark
    /// runner warns when this is large, because a noisy measurement should not
    /// be quietly reported as if it were precise.
    [[nodiscard]] double relative_mad() const noexcept {
        return median == 0.0 ? 0.0 : mad / median;
    }

    /// True when the mean sits far from the median, indicating skew or
    /// outliers. Recorded in results so a disturbed run is visible rather than
    /// averaged away.
    [[nodiscard]] bool mean_median_diverges(double threshold = 0.10) const noexcept {
        return median != 0.0 && std::fabs(mean - median) / median > threshold;
    }
};

namespace detail {

/// Linear-interpolated quantile of an already-sorted sample.
///
/// Interpolating rather than picking the nearest order statistic matters at the
/// repetition counts used here: with 11 samples the nearest-rank p95 is just
/// the maximum, which would make p95 and max identical and uninformative.
[[nodiscard]] inline double quantile_sorted(const std::vector<double>& sorted, double q) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1U) {
        return sorted[0];
    }
    const double pos   = q * static_cast<double>(sorted.size() - 1U);
    const auto   lo    = static_cast<std::size_t>(std::floor(pos));
    const auto   hi    = static_cast<std::size_t>(std::ceil(pos));
    const double frac  = pos - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

}  // namespace detail

/// Median of an already-sorted sample.
[[nodiscard]] inline double median_sorted(const std::vector<double>& sorted) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t n = sorted.size();
    if (n % 2U == 1U) {
        return sorted[n / 2U];
    }
    // Averaging the two central values, computed as a midpoint rather than
    // (a + b) / 2 so that large timings cannot overflow into imprecision.
    const double a = sorted[n / 2U - 1U];
    const double b = sorted[n / 2U];
    return a + (b - a) * 0.5;
}

/// Summarises `samples`. The input is copied, so the caller's raw data -- which
/// must be retained for the result record -- is not reordered.
[[nodiscard]] inline SampleSummary summarise(const std::vector<double>& samples) {
    SampleSummary s;
    s.count = samples.size();
    if (samples.empty()) {
        return s;
    }

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    s.median = median_sorted(sorted);
    s.min    = sorted.front();
    s.max    = sorted.back();
    s.p25    = detail::quantile_sorted(sorted, 0.25);
    s.p75    = detail::quantile_sorted(sorted, 0.75);
    s.p95    = detail::quantile_sorted(sorted, 0.95);
    s.p99    = detail::quantile_sorted(sorted, 0.99);

    // --- MAD ---------------------------------------------------------------
    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    for (double v : sorted) {
        deviations.push_back(std::fabs(v - s.median));
    }
    std::sort(deviations.begin(), deviations.end());
    s.mad        = median_sorted(deviations);
    s.mad_scaled = s.mad * 1.4826;

    // --- Mean and standard deviation ---------------------------------------
    double sum = 0.0;
    for (double v : samples) {
        sum += v;
    }
    s.mean = sum / static_cast<double>(samples.size());

    if (samples.size() > 1U) {
        // Two-pass about the mean rather than the raw second moment: timings
        // are large numbers with small relative spread, exactly the case where
        // the one-pass formula loses the variance to cancellation.
        double sq = 0.0;
        for (double v : samples) {
            const double d = v - s.mean;
            sq += d * d;
        }
        // Sample variance (n - 1): these repetitions are a sample of possible
        // runs, not the population of them.
        s.stddev = std::sqrt(sq / static_cast<double>(samples.size() - 1U));
    }

    return s;
}

/// Convenience overload for integer nanosecond timings.
[[nodiscard]] inline SampleSummary summarise_ns(const std::vector<std::uint64_t>& samples) {
    std::vector<double> as_double;
    as_double.reserve(samples.size());
    for (std::uint64_t v : samples) {
        as_double.push_back(static_cast<double>(v));
    }
    return summarise(as_double);
}

}  // namespace lpq

#endif  // LPQ_METRICS_STATISTICS_HPP
