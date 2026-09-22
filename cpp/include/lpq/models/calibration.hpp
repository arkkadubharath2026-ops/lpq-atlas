// =============================================================================
// Confidence calibration (PRD.md section 12).
//
// PRD.md section 12 requires the predictor to expose a score, a confidence and
// an uncertainty, and states that "confidence must be calibrated and
// evaluated". This header provides the online, in-engine half of that; the
// offline half (reliability curves, isotonic regression) lives in the Python
// analysis tooling, for reasons given at the bottom.
//
// -----------------------------------------------------------------------------
// Why calibration is not cosmetic here
// -----------------------------------------------------------------------------
//
// The adaptation controller (PRD.md section 14) decides how far to trust a
// prediction by comparing its confidence against a threshold. That comparison
// is only meaningful if confidence means what it says: of the candidates a
// model calls 0.9, about nine in ten should actually be the correct next
// extraction.
//
// A raw sigmoid output does not have that property. A logistic model trained on
// the natural label distribution -- one positive per extraction against many
// negatives -- is systematically overconfident, so a threshold of 0.9 admits
// far more than a one-in-ten error rate. The controller would then be
// configured with a number that does not describe the behaviour it produces,
// and every tuning experiment downstream would be measuring the miscalibration
// rather than the policy.
//
// -----------------------------------------------------------------------------
// Temperature scaling
// -----------------------------------------------------------------------------
//
// A single scalar T divides the logit before the sigmoid. T > 1 softens
// confidence towards 0.5; T < 1 sharpens it.
//
// One parameter was chosen deliberately over richer alternatives. Temperature
// scaling cannot change the ORDER of any two predictions -- it is monotone in
// the logit -- so it adjusts confidence without touching the ranking the model
// produces. That separation matters: ranking quality and confidence quality can
// then be measured and fixed independently, and recalibrating can never
// silently degrade the ordering that the queue actually uses.
//
// It also costs one multiply in the hot path, which a per-bin lookup or an
// isotonic step function would not.
//
// -----------------------------------------------------------------------------
// What is deliberately offline
// -----------------------------------------------------------------------------
//
// Isotonic calibration and reliability curves need a held-out sample and a full
// pass over it. Both are genuinely useful and both are listed in PRD.md section
// 12, but neither belongs in the search loop: they are batch procedures over
// accumulated events, which is exactly what the Python evaluation tooling is
// for. The C++ side accumulates the (confidence, outcome) pairs that those
// procedures consume -- see ReliabilityAccumulator below -- so the offline step
// has real data rather than a reconstruction.
// =============================================================================
#ifndef LPQ_MODELS_CALIBRATION_HPP
#define LPQ_MODELS_CALIBRATION_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "lpq/common/error.hpp"

namespace lpq {

/// Numerically stable logistic function.
///
/// The naive form 1/(1+exp(-z)) overflows exp() for large negative z and
/// returns inf, then 1/inf = 0, which happens to be the right answer -- but it
/// raises a floating-point overflow along the way and is only accidentally
/// correct. The branched form never evaluates exp() on a positive argument, so
/// the intermediate is always in (0, 1].
[[nodiscard]] inline float logistic(float z) noexcept {
    if (z >= 0.0F) {
        return 1.0F / (1.0F + std::exp(-z));
    }
    const float e = std::exp(z);
    return e / (1.0F + e);
}

/// Inverse of logistic(), clamped away from the asymptotes.
///
/// A confidence of exactly 0 or 1 has an infinite logit. Clamping to a value
/// representable in float keeps the inverse finite, which matters because an
/// infinity here would propagate into a weight update and destroy the model
/// rather than merely perturbing it.
[[nodiscard]] inline float logit(float p) noexcept {
    constexpr float kEps = 1e-7F;
    const float clamped = p < kEps ? kEps : (p > 1.0F - kEps ? 1.0F - kEps : p);
    return std::log(clamped / (1.0F - clamped));
}

/// Single-parameter temperature scaling.
class TemperatureScaler {
public:
    TemperatureScaler() = default;

    explicit TemperatureScaler(float temperature) { set_temperature(temperature); }

    /// Rejects a non-positive temperature rather than clamping it.
    ///
    /// Zero would divide by zero and a negative value would INVERT every
    /// confidence, turning the model's most certain positives into its most
    /// certain negatives while leaving every number in a plausible range. That
    /// is precisely the class of fault that is invisible downstream, so it is
    /// refused at the point it can still be attributed.
    void set_temperature(float t) {
        if (!(t > 0.0F) || !std::isfinite(t)) {
            throw InvalidConfigError(
                "calibration temperature must be finite and strictly positive");
        }
        inv_temperature_ = 1.0F / t;
    }

    [[nodiscard]] float temperature() const noexcept { return 1.0F / inv_temperature_; }

    /// Maps a raw logit to a calibrated probability.
    [[nodiscard]] float calibrate_logit(float z) const noexcept {
        return logistic(z * inv_temperature_);
    }

    /// Maps an already-sigmoided probability to a calibrated one.
    [[nodiscard]] float calibrate_probability(float p) const noexcept {
        return calibrate_logit(logit(p));
    }

    [[nodiscard]] bool is_identity() const noexcept { return inv_temperature_ == 1.0F; }

private:
    float inv_temperature_ = 1.0F;  ///< stored inverted: the hot path multiplies
};

/// How far a confidence is from being a decision, in [0, 1].
///
/// 0.5 is maximal uncertainty and both 0 and 1 are decisive. This is the
/// "uncertainty or margin" PRD.md section 12 requires, expressed so that larger
/// always means less certain regardless of which class is predicted.
[[nodiscard]] inline float uncertainty_from_confidence(float p) noexcept {
    const float margin = p > 0.5F ? (p - 0.5F) : (0.5F - p);
    return 1.0F - (2.0F * margin);
}

/// Accumulates (confidence, outcome) pairs into equal-width bins.
///
/// This is the raw material for a reliability diagram and for the expected
/// calibration error. It is kept in C++ rather than reconstructed in Python
/// because the confidences a model produced during a timed run are not
/// recoverable afterwards -- replaying the search would produce a different
/// sequence once online updates have moved the weights.
///
/// Bins are fixed-width and fixed-count so the structure is a flat array with
/// no allocation and the update is an index computation; it is written to from
/// the verification path, which runs once per extraction.
template <std::size_t Bins = 10>
class ReliabilityAccumulator {
public:
    static_assert(Bins >= 2, "a reliability diagram needs at least two bins");

    /// Records one prediction and the verified outcome it turned out to have.
    void record(float confidence, bool was_correct) noexcept {
        const std::size_t b = bin_of(confidence);
        ++count_[b];
        confidence_sum_[b] += static_cast<double>(confidence);
        if (was_correct) {
            ++positive_[b];
        }
    }

    [[nodiscard]] std::uint64_t count(std::size_t b) const noexcept { return count_[b]; }

    /// Observed frequency of correctness in a bin, or 0 for an empty bin.
    [[nodiscard]] double observed_frequency(std::size_t b) const noexcept {
        return count_[b] == 0 ? 0.0
                              : static_cast<double>(positive_[b]) / static_cast<double>(count_[b]);
    }

    /// Mean predicted confidence in a bin, or 0 for an empty bin.
    ///
    /// The bin's own mean rather than its midpoint: with skewed confidences a
    /// bin's contents can cluster at one edge, and using the midpoint would
    /// attribute a calibration error to the model that is really an artefact of
    /// the binning.
    [[nodiscard]] double mean_confidence(std::size_t b) const noexcept {
        return count_[b] == 0 ? 0.0 : confidence_sum_[b] / static_cast<double>(count_[b]);
    }

    [[nodiscard]] std::uint64_t total() const noexcept {
        std::uint64_t t = 0;
        for (std::size_t b = 0; b < Bins; ++b) {
            t += count_[b];
        }
        return t;
    }

    /// Expected calibration error: the count-weighted mean gap between
    /// predicted confidence and observed frequency.
    ///
    /// Returns 0 for an empty accumulator, which is the only honest answer --
    /// no predictions means no evidence of miscalibration, not evidence of
    /// good calibration. Callers distinguish the two by checking total().
    [[nodiscard]] double expected_calibration_error() const noexcept {
        const std::uint64_t n = total();
        if (n == 0) {
            return 0.0;
        }
        double err = 0.0;
        for (std::size_t b = 0; b < Bins; ++b) {
            if (count_[b] == 0) {
                continue;
            }
            const double gap = mean_confidence(b) - observed_frequency(b);
            err += static_cast<double>(count_[b]) * (gap < 0.0 ? -gap : gap);
        }
        return err / static_cast<double>(n);
    }

    void reset() noexcept {
        count_.fill(0);
        positive_.fill(0);
        confidence_sum_.fill(0.0);
    }

    [[nodiscard]] static constexpr std::size_t bin_count() noexcept { return Bins; }

private:
    /// Maps a confidence to its bin, tolerating values slightly outside [0, 1].
    ///
    /// A confidence of exactly 1.0 would index one past the last bin under a
    /// plain multiply-and-truncate, so it is folded into the top bin.
    [[nodiscard]] static std::size_t bin_of(float c) noexcept {
        if (!(c > 0.0F)) {
            return 0;  // also catches NaN, which must not index the array
        }
        if (c >= 1.0F) {
            return Bins - 1;
        }
        const std::size_t b = static_cast<std::size_t>(c * static_cast<float>(Bins));
        return b >= Bins ? Bins - 1 : b;
    }

    std::array<std::uint64_t, Bins> count_{};
    std::array<std::uint64_t, Bins> positive_{};
    std::array<double, Bins>        confidence_sum_{};
};

}  // namespace lpq

#endif  // LPQ_MODELS_CALIBRATION_HPP
