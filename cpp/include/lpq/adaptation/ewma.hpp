// =============================================================================
// Exponentially weighted statistics and exact small-range quantiles
// (ESD.md section 18, PRD.md section 14).
//
// ESD.md section 18 requires the adaptive controller to maintain exponentially
// weighted statistics -- prediction success rate, mean rank error, p95 rank
// error, correction rate, timings, queue size -- and the online learner needs
// the same recent-error signal to decide when an adaptive update is due.
//
// -----------------------------------------------------------------------------
// Why exponentially weighted rather than a running mean
// -----------------------------------------------------------------------------
//
// A running mean over a whole search is the wrong statistic for a controller.
// PRD.md section 15 makes graceful degradation under DISTRIBUTION SHIFT a
// primary research goal, and a mean over all history responds to a shift at a
// rate that slows as the run goes on: after a million events, a thousand bad
// ones move it by a thousandth. The controller would keep trusting a model that
// had already stopped working.
//
// An exponentially weighted statistic has a fixed response time regardless of
// how long the run has been going, which is exactly the property a controller
// that must react to degradation needs.
//
// -----------------------------------------------------------------------------
// Exact quantiles, not estimated ones
// -----------------------------------------------------------------------------
//
// The usual way to track a p95 online is an approximate estimator (P-squared,
// t-digest) because the value range is unbounded. Here it is not: rank error is
// bounded by the candidate window, which never exceeds kMaxSupportedWindow. A
// plain histogram over that range therefore gives the EXACT quantile in O(k),
// with no estimator error to explain and no accuracy that degrades on skewed
// input -- and skew is precisely what a tail statistic is being consulted about.
// =============================================================================
#ifndef LPQ_ADAPTATION_EWMA_HPP
#define LPQ_ADAPTATION_EWMA_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "lpq/common/error.hpp"

namespace lpq {

/// An exponentially weighted mean.
class ExponentiallyWeighted {
public:
    ExponentiallyWeighted() = default;

    /// `alpha` is the weight given to each new observation, in (0, 1].
    /// Larger reacts faster and is noisier.
    explicit ExponentiallyWeighted(double alpha) { set_alpha(alpha); }

    void set_alpha(double alpha) {
        if (!(alpha > 0.0) || !(alpha <= 1.0) || !std::isfinite(alpha)) {
            throw InvalidConfigError("EWMA alpha must be finite and within (0, 1]");
        }
        alpha_ = alpha;
    }

    [[nodiscard]] double alpha() const noexcept { return alpha_; }

    /// Adds an observation.
    ///
    /// The first observation becomes the value outright rather than being
    /// blended with a zero that was never measured. Seeding from zero would
    /// make the statistic report a low error simply because it had not seen
    /// anything yet, and a controller reading it would enter learned mode on
    /// the strength of no evidence at all.
    void add(double x) noexcept {
        if (count_ == 0) {
            value_ = x;
        } else {
            value_ = (alpha_ * x) + ((1.0 - alpha_) * value_);
        }
        ++count_;
    }

    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    /// Whether enough observations have accumulated for the value to mean
    /// something. Callers gate decisions on this rather than on the value.
    [[nodiscard]] bool warm(std::uint64_t minimum) const noexcept { return count_ >= minimum; }

    void reset() noexcept {
        value_ = 0.0;
        count_ = 0;
    }

private:
    double        alpha_ = 0.05;
    double        value_ = 0.0;
    std::uint64_t count_ = 0;
};

/// Exact quantiles over a small, bounded integer range.
///
/// Sized for rank errors, which cannot exceed the candidate window. A value at
/// or above the capacity is folded into the top bucket rather than dropped, so
/// the reported tail is conservative -- it can understate how far out an
/// outlier was, but never pretends there was none.
template <std::size_t Range = 64>
class BoundedHistogram {
public:
    static_assert(Range >= 2, "a histogram needs at least two buckets");

    void add(std::uint32_t value) noexcept {
        const std::size_t at = value >= Range ? Range - 1 : static_cast<std::size_t>(value);
        ++counts_[at];
        ++total_;
        if (value > max_) {
            max_ = value;
        }
    }

    /// The smallest value at or below which at least `q` of the mass lies.
    ///
    /// Returns 0 for an empty histogram, which callers distinguish from a real
    /// zero with total().
    [[nodiscard]] std::uint32_t quantile(double q) const noexcept {
        if (total_ == 0) {
            return 0;
        }
        const double  target = q * static_cast<double>(total_);
        std::uint64_t seen   = 0;
        for (std::size_t i = 0; i < Range; ++i) {
            seen += counts_[i];
            if (static_cast<double>(seen) >= target) {
                return static_cast<std::uint32_t>(i);
            }
        }
        return static_cast<std::uint32_t>(Range - 1);
    }

    [[nodiscard]] double mean() const noexcept {
        if (total_ == 0) {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < Range; ++i) {
            sum += static_cast<double>(i) * static_cast<double>(counts_[i]);
        }
        return sum / static_cast<double>(total_);
    }

    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }

    /// The largest value ever observed, including values folded into the top
    /// bucket. Reported separately so the fold is visible rather than silent.
    [[nodiscard]] std::uint32_t maximum() const noexcept { return max_; }

    [[nodiscard]] std::uint64_t count_at(std::size_t bucket) const noexcept {
        return bucket < Range ? counts_[bucket] : 0;
    }

    void reset() noexcept {
        counts_.fill(0);
        total_ = 0;
        max_   = 0;
    }

    [[nodiscard]] static constexpr std::size_t range() noexcept { return Range; }

private:
    std::array<std::uint64_t, Range> counts_{};
    std::uint64_t                    total_ = 0;
    std::uint32_t                    max_   = 0;
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_EWMA_HPP
