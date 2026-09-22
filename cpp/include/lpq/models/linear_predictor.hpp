// =============================================================================
// Linear predictor (ESD.md section 12, PRD.md section 10: "Linear model").
//
//     score(x) = w . x + b
//
// -----------------------------------------------------------------------------
// Why a linear model is the serious candidate, not the warm-up
// -----------------------------------------------------------------------------
//
// PRD.md section 10 is explicit that "the largest model is NOT automatically
// the best model" and that the project "should prefer the simplest model that
// wins on the target workload". For this workload the linear model is a
// genuine contender rather than a stepping stone, because of where it sits in
// the cost equation PRD.md section 10 gives:
//
//     end-to-end = queue + feature + INFERENCE + verification + traversal
//
// Inference runs once per candidate evaluation. A dot product over 28 floats is
// a handful of FMAs over two cache lines and vectorises without help. An MLP
// with a 32-unit hidden layer is roughly thirty times that arithmetic plus a
// nonlinearity, for a model that must then beat the linear one by enough to pay
// for the difference. A model that predicts better and loses on wall-clock has
// lost.
//
// -----------------------------------------------------------------------------
// Determinism of the dot product
// -----------------------------------------------------------------------------
//
// Floating-point addition is not associative, so a sum's value depends on the
// order the terms are added. Two builds that vectorise the loop differently
// would produce slightly different scores, and near a tie those differences
// change the ORDER of two candidates -- which changes the search path, the
// number of settled nodes, and the measured runtime.
//
// The loop is therefore a plain sequential accumulation, and the build passes
// -fno-fast-math (see cmake/LpqCompilerSettings.cmake) so the compiler may not
// reassociate it. The accumulator is double rather than float: the extra
// precision costs nothing measurable at this length and keeps the running sum
// from losing the small terms that decide close comparisons.
//
// -----------------------------------------------------------------------------
// Online update
// -----------------------------------------------------------------------------
//
// Squared-loss SGD, which makes this the regression formulation PRD.md section
// 8 asks for as an ablation ("a direct raw-distance predictor may be
// implemented as an experimental ablation"). The classification formulation is
// LogisticPredictor.
// =============================================================================
#ifndef LPQ_MODELS_LINEAR_PREDICTOR_HPP
#define LPQ_MODELS_LINEAR_PREDICTOR_HPP

#include <cmath>
#include <array>
#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/calibration.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

namespace detail {

/// Sequential, double-accumulated dot product. See the header on determinism.
[[nodiscard]] inline float dot(const std::array<float, kFeatureCount>& w,
                               const FeatureVector& x) noexcept {
    double acc = 0.0;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        acc += static_cast<double>(w[i]) * static_cast<double>(x[i]);
    }
    return static_cast<float>(acc);
}

}  // namespace detail

/// Hyperparameters shared by the linear and logistic models.
struct LinearTrainingConfig {
    /// SGD step size.
    float learning_rate = 0.01F;

    /// L2 penalty, applied as decay towards zero on each update.
    ///
    /// Non-zero by default. Online learning here runs on a stream whose
    /// distribution shifts as the search moves through the graph, and an
    /// unregularised model will chase the most recent region and forget the
    /// rest. Decay bounds how far the weights can drift from zero, which bounds
    /// how badly a transient region of the graph can corrupt the model.
    float l2 = 1e-4F;

    /// Multiplier applied to the loss of positive examples.
    ///
    /// The label stream is severely imbalanced: exactly one candidate per
    /// extraction is the true minimum. Without reweighting, a model that always
    /// answers "no" achieves excellent average loss and is useless. See
    /// TrainingExample::weight.
    float positive_weight = 1.0F;

    /// Caps the per-update change to any single weight.
    ///
    /// A single pathological example -- an unnormalised feature, a
    /// mislabelled event -- can otherwise move a weight far enough to destroy
    /// the model in one step. Because updates happen inside a running search,
    /// there is no epoch boundary at which such damage would be noticed, so the
    /// bound is enforced per update rather than repaired later. This is also
    /// the "bounded update cost" PRD.md section 11 requires, in its numerical
    /// sense.
    float max_weight_delta = 1.0F;

    void validate() const {
        if (!std::isfinite(learning_rate) || learning_rate < 0.0F) {
            throw InvalidConfigError("learning_rate must be finite and non-negative");
        }
        if (!std::isfinite(l2) || l2 < 0.0F) {
            throw InvalidConfigError("l2 must be finite and non-negative");
        }
        if (!std::isfinite(positive_weight) || positive_weight <= 0.0F) {
            throw InvalidConfigError("positive_weight must be finite and positive");
        }
        if (!std::isfinite(max_weight_delta) || max_weight_delta <= 0.0F) {
            throw InvalidConfigError("max_weight_delta must be finite and positive");
        }
    }
};

/// score(x) = w . x + b, with squared-loss online updates.
class LinearPredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kLinear;
    static constexpr std::string_view kName = "linear";

    LinearPredictor() { weights_.fill(0.0F); }

    explicit LinearPredictor(LinearTrainingConfig config) : config_(config) {
        config_.validate();
        weights_.fill(0.0F);
    }

    // --- Inference -----------------------------------------------------------

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        const float raw = detail::dot(weights_, features) + bias_;

        Prediction p;
        p.score = raw;
        // A linear score has no natural probabilistic reading, so confidence
        // comes from pushing it through the calibrator. This is honest only
        // because the calibrator is fitted against observed outcomes; an
        // uncalibrated sigmoid of an arbitrary-scale score would be a number
        // shaped like a probability that means nothing.
        p.confidence     = calibrator_.calibrate_logit(raw);
        p.uncertainty    = uncertainty_from_confidence(p.confidence);
        p.estimated_rank = kUnknownRank;
        return p;
    }

    // --- Online update -------------------------------------------------------

    /// One squared-loss SGD step.
    void update(const TrainingExample& example) noexcept {
        const float raw   = detail::dot(weights_, example.features) + bias_;
        const float error = raw - example.target;

        // A non-finite example must not be allowed to poison the weights.
        // Feature extraction guards against this at source, but an update can
        // also arrive from a deserialised event log or a Python-generated
        // dataset, neither of which this model controls.
        if (!std::isfinite(error)) {
            return;
        }

        const float w = example_weight(example);
        apply_gradient(example.features, error * w);
    }

    [[nodiscard]] bool supports_online_update() const noexcept { return true; }

    // --- Parameters ----------------------------------------------------------

    [[nodiscard]] const std::array<float, kFeatureCount>& weights() const noexcept {
        return weights_;
    }
    [[nodiscard]] float bias() const noexcept { return bias_; }

    void set_weight(Feature f, float v) noexcept {
        weights_[static_cast<std::size_t>(f)] = v;
    }
    void set_bias(float v) noexcept { bias_ = v; }

    /// Installs weights from an offline (Python) fit. See ESD.md section 13.
    void set_weights(const std::vector<float>& w) {
        if (w.size() != kFeatureCount) {
            throw InvalidConfigError("weight vector has " + std::to_string(w.size()) +
                                     " entries but the feature layout has " +
                                     std::to_string(kFeatureCount));
        }
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            weights_[i] = w[i];
        }
    }

    [[nodiscard]] const LinearTrainingConfig& config() const noexcept { return config_; }
    void set_config(LinearTrainingConfig c) {
        c.validate();
        config_ = c;
    }

    [[nodiscard]] const TemperatureScaler& calibrator() const noexcept { return calibrator_; }
    void set_calibrator(TemperatureScaler c) noexcept { calibrator_ = c; }

    [[nodiscard]] std::uint64_t updates_applied() const noexcept { return updates_; }

    // --- Serialization -------------------------------------------------------

    void save(std::ostream& os) const { save_as(os, kKind); }

    [[nodiscard]] static LinearPredictor load(std::istream& is) {
        LinearPredictor m;
        m.load_body(is, read_model_header_expecting(is, kKind));
        return m;
    }

protected:
    // Shared with LogisticPredictor, which differs only in its update rule and
    // in how it reads its score. Duplicating the parameter block and its
    // serialization in both would mean two places to keep in step with the
    // feature layout.

    /// Writes the parameter block under a caller-supplied kind tag.
    void save_as(std::ostream& os, ModelKind kind) const {
        write_model_header(os, kind);
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            detail::put_f32(os, weights_[i]);
        }
        detail::put_f32(os, bias_);
        detail::put_f32(os, calibrator_.temperature());
        detail::put_f32(os, config_.learning_rate);
        detail::put_f32(os, config_.l2);
        detail::put_f32(os, config_.positive_weight);
        detail::put_f32(os, config_.max_weight_delta);
        detail::put_u64(os, updates_);
    }

    void load_body(std::istream& is, const ModelHeader& header) {
        (void)header;  // already validated by the caller
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            weights_[i] = detail::get_f32(is);
        }
        bias_ = detail::get_f32(is);

        // Validated rather than assigned: a corrupt temperature would silently
        // invert every confidence (see TemperatureScaler::set_temperature).
        const float temperature = detail::get_f32(is);
        calibrator_.set_temperature(temperature);

        config_.learning_rate    = detail::get_f32(is);
        config_.l2               = detail::get_f32(is);
        config_.positive_weight  = detail::get_f32(is);
        config_.max_weight_delta = detail::get_f32(is);
        config_.validate();

        updates_ = detail::get_u64(is);

        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            if (!std::isfinite(weights_[i])) {
                throw ModelSerializationError("model file contains a non-finite weight at index " +
                                              std::to_string(i));
            }
        }
        if (!std::isfinite(bias_)) {
            throw ModelSerializationError("model file contains a non-finite bias");
        }
    }

    /// Weight for one example: the example's own weight, times the positive
    /// class multiplier when the example is a positive.
    [[nodiscard]] float example_weight(const TrainingExample& example) const noexcept {
        const float base = std::isfinite(example.weight) && example.weight > 0.0F
                               ? example.weight
                               : 1.0F;
        return example.target > 0.5F ? base * config_.positive_weight : base;
    }

    /// Applies -lr * scaled_gradient with L2 decay and per-weight clipping.
    void apply_gradient(const FeatureVector& x, float scaled_error) noexcept {
        const float lr    = config_.learning_rate;
        const float decay = 1.0F - (lr * config_.l2);
        const float cap   = config_.max_weight_delta;

        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            float delta = lr * scaled_error * x[i];
            if (!std::isfinite(delta)) {
                continue;
            }
            delta = delta > cap ? cap : (delta < -cap ? -cap : delta);
            weights_[i] = (weights_[i] * decay) - delta;
        }

        // The bias is decayed too, but is NOT subject to the same clipping
        // rationale as the weights: it multiplies a constant 1 rather than a
        // feature, so it cannot be driven by an outlying input value. It is
        // still clipped, because a runaway error term reaches it as well.
        float bias_delta = lr * scaled_error;
        if (std::isfinite(bias_delta)) {
            bias_delta = bias_delta > cap ? cap : (bias_delta < -cap ? -cap : bias_delta);
            bias_      = (bias_ * decay) - bias_delta;
        }
        ++updates_;
    }

    std::array<float, kFeatureCount> weights_{};
    float                            bias_ = 0.0F;
    LinearTrainingConfig             config_{};
    TemperatureScaler                calibrator_{};
    std::uint64_t                    updates_ = 0;
};

}  // namespace lpq

#endif  // LPQ_MODELS_LINEAR_PREDICTOR_HPP
