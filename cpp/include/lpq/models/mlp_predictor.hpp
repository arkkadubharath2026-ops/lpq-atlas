// =============================================================================
// Small MLP predictor (ESD.md section 12: "Small MLP if experimentally
// justified", PRD.md section 10: "Small MLP").
//
//     logit(x) = W2 . relu(W1 . x + b1) + b2
//
// One hidden layer, ReLU, scalar output.
//
// -----------------------------------------------------------------------------
// "If experimentally justified"
// -----------------------------------------------------------------------------
//
// ESD.md section 12 attaches that condition, and it is a real one. This model
// exists so the comparison can be MADE, not because it is expected to win.
//
// The arithmetic makes the bar concrete. The linear model is 28 multiply-adds.
// This model with a 32-unit hidden layer is 28*32 + 32 = 928, plus 32 ReLUs --
// about thirty times the work, per candidate, several million times per query.
// For the MLP to be the right production choice it must not merely predict
// better; it must predict enough better that the queue work it saves exceeds
// the inference time it adds. PRD.md section 10 states this directly: "the
// largest model is NOT automatically the best model".
//
// The honest outcome is therefore that this model is implemented, benchmarked
// alongside the others, and selected only if the end-to-end measurement
// supports it. Until that measurement exists, no claim is made about it either
// way.
//
// -----------------------------------------------------------------------------
// Fixed capacity, no allocation
// -----------------------------------------------------------------------------
//
// The hidden layer is bounded at compile time and the activations live in a
// std::array in predict()'s own stack frame. The alternative -- a member
// scratch buffer -- would make predict() mutate the object, which would break
// the const contract that keeps inference order-independent and thread-safe
// (see predictor.hpp). Allocating per call is not an option at this call rate.
//
// The cap is 64 units. That is not a limit on what the architecture can
// express so much as a statement about what is affordable here: a hidden layer
// larger than 64 costs more than 1800 multiply-adds per candidate, which on
// this workload is already far past the point where the model could pay for
// itself. A larger network is not silently truncated; construction throws.
// =============================================================================
#ifndef LPQ_MODELS_MLP_PREDICTOR_HPP
#define LPQ_MODELS_MLP_PREDICTOR_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/calibration.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// Largest hidden layer this build will run. See the header.
inline constexpr std::size_t kMaxHiddenUnits = 64;

/// One hidden layer, ReLU activation, scalar logit output.
class MlpPredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kMlp;
    static constexpr std::string_view kName = "mlp";

    MlpPredictor() : MlpPredictor(16) {}

    explicit MlpPredictor(std::size_t hidden_units) { resize(hidden_units); }

    MlpPredictor(std::size_t hidden_units, LinearTrainingConfig config) : config_(config) {
        config_.validate();
        resize(hidden_units);
    }

    // --- Initialisation ------------------------------------------------------

    /// He-style initialisation from a seeded RNG.
    ///
    /// Zero-initialising a hidden layer is not merely a poor start, it is a
    /// broken one: every hidden unit would compute the same function and
    /// receive the same gradient forever, so the network would have the
    /// capacity of a single unit no matter how wide it was. The scale
    /// sqrt(2/fan_in) is the standard choice for ReLU, which zeroes half its
    /// inputs and so halves the variance a plain unit-variance init would give.
    ///
    /// The RNG is passed in rather than created here so initialisation is
    /// reproducible from the experiment's master seed, like everything else in
    /// this project.
    void initialise(Rng& rng) {
        const double scale = std::sqrt(2.0 / static_cast<double>(kFeatureCount));
        for (float& w : w1_) {
            w = static_cast<float>(rng.next_normal() * scale);
        }
        const double scale2 = std::sqrt(2.0 / static_cast<double>(hidden_units_));
        for (float& w : w2_) {
            w = static_cast<float>(rng.next_normal() * scale2);
        }
        // Biases start at zero: they are not subject to the symmetry problem,
        // because the weights above already break it.
        for (float& b : b1_) {
            b = 0.0F;
        }
        b2_ = 0.0F;
    }

    // --- Inference -----------------------------------------------------------

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        std::array<float, kMaxHiddenUnits> hidden{};
        const float z = forward(features, hidden);

        Prediction p;
        p.score          = z;
        p.confidence     = calibrator_.calibrate_logit(z);
        p.uncertainty    = uncertainty_from_confidence(p.confidence);
        p.estimated_rank = kUnknownRank;
        return p;
    }

    // --- Online update -------------------------------------------------------

    /// One backpropagation step on log-loss.
    ///
    /// Backpropagation through one hidden layer is cheap enough to run online,
    /// but it is roughly twice the cost of the forward pass. PRD.md section 11
    /// permits batching updates between queries if the per-step cost becomes a
    /// problem; whether it does is a measurement, not an assumption, so the
    /// straightforward per-event update is what is implemented.
    void update(const TrainingExample& example) noexcept {
        std::array<float, kMaxHiddenUnits> hidden{};
        const float z = forward(example.features, hidden);
        if (!std::isfinite(z)) {
            return;
        }

        const float output_error =
            (logistic(z) - example.target) * example_weight(example);
        if (!std::isfinite(output_error)) {
            return;
        }

        const float lr    = config_.learning_rate;
        const float decay = 1.0F - (lr * config_.l2);
        const float cap   = config_.max_weight_delta;

        auto clip = [cap](float d) noexcept {
            return d > cap ? cap : (d < -cap ? -cap : d);
        };

        // Output layer. The hidden activations are read before they are
        // overwritten, which is why the hidden-layer gradient is accumulated
        // into a separate array below rather than applied in place.
        std::array<float, kMaxHiddenUnits> hidden_error{};
        for (std::size_t j = 0; j < hidden_units_; ++j) {
            // Gradient w.r.t. the hidden activation, needed before w2_ moves.
            hidden_error[j] = output_error * w2_[j];

            const float d = clip(lr * output_error * hidden[j]);
            w2_[j]        = (w2_[j] * decay) - (std::isfinite(d) ? d : 0.0F);
        }
        const float bd = clip(lr * output_error);
        b2_            = (b2_ * decay) - (std::isfinite(bd) ? bd : 0.0F);

        // Hidden layer. ReLU's derivative is 1 where the pre-activation was
        // positive and 0 elsewhere, so a unit that did not fire receives no
        // gradient and is skipped entirely -- which is also why the loop below
        // is much cheaper than the forward pass on a sparse activation.
        for (std::size_t j = 0; j < hidden_units_; ++j) {
            if (hidden[j] <= 0.0F) {
                continue;
            }
            const float g = hidden_error[j];
            if (!std::isfinite(g)) {
                continue;
            }
            float* row = &w1_[j * kFeatureCount];
            for (std::size_t i = 0; i < kFeatureCount; ++i) {
                const float d = clip(lr * g * example.features[i]);
                row[i]        = (row[i] * decay) - (std::isfinite(d) ? d : 0.0F);
            }
            const float hd = clip(lr * g);
            b1_[j]         = (b1_[j] * decay) - (std::isfinite(hd) ? hd : 0.0F);
        }
        ++updates_;
    }

    [[nodiscard]] bool supports_online_update() const noexcept { return true; }

    // --- Parameters ----------------------------------------------------------

    [[nodiscard]] std::size_t hidden_units() const noexcept { return hidden_units_; }
    [[nodiscard]] std::uint64_t updates_applied() const noexcept { return updates_; }

    /// Multiply-adds per inference. Used by the model-cost comparison, where an
    /// architecture-independent operation count is more portable than a timing.
    [[nodiscard]] std::size_t inference_flops() const noexcept {
        return (kFeatureCount * hidden_units_) + hidden_units_;
    }

    [[nodiscard]] const std::vector<float>& hidden_weights() const noexcept { return w1_; }
    [[nodiscard]] const std::vector<float>& output_weights() const noexcept { return w2_; }

    /// Installs parameters from an offline (Python) fit. See ESD.md section 13.
    ///
    /// w1 is row-major with one row per hidden unit.
    void set_parameters(const std::vector<float>& w1, const std::vector<float>& b1,
                        const std::vector<float>& w2, float b2) {
        if (b1.size() != hidden_units_ || w2.size() != hidden_units_) {
            throw InvalidConfigError("MLP bias/output vectors do not match the hidden width");
        }
        if (w1.size() != hidden_units_ * kFeatureCount) {
            throw InvalidConfigError("MLP hidden weight matrix does not match hidden width times "
                                     "feature count");
        }
        w1_ = w1;
        b1_ = b1;
        w2_ = w2;
        b2_ = b2;
    }

    [[nodiscard]] const TemperatureScaler& calibrator() const noexcept { return calibrator_; }
    void set_calibrator(TemperatureScaler c) noexcept { calibrator_ = c; }

    [[nodiscard]] const LinearTrainingConfig& config() const noexcept { return config_; }
    void set_config(LinearTrainingConfig c) {
        c.validate();
        config_ = c;
    }

    // --- Serialization -------------------------------------------------------

    void save(std::ostream& os) const {
        write_model_header(os, kKind);
        detail::put_u64(os, static_cast<std::uint64_t>(hidden_units_));
        detail::put_f32_array(os, w1_);
        detail::put_f32_array(os, b1_);
        detail::put_f32_array(os, w2_);
        detail::put_f32(os, b2_);
        detail::put_f32(os, calibrator_.temperature());
        detail::put_f32(os, config_.learning_rate);
        detail::put_f32(os, config_.l2);
        detail::put_f32(os, config_.positive_weight);
        detail::put_f32(os, config_.max_weight_delta);
        detail::put_u64(os, updates_);
    }

    [[nodiscard]] static MlpPredictor load(std::istream& is) {
        (void)read_model_header_expecting(is, kKind);

        const std::uint64_t hidden = detail::get_u64(is);
        // Checked before anything is sized from it, for the same reason
        // get_f32_array caps its length.
        if (hidden == 0 || hidden > kMaxHiddenUnits) {
            throw ModelSerializationError("model declares " + std::to_string(hidden) +
                                          " hidden units, outside the supported range 1.." +
                                          std::to_string(kMaxHiddenUnits));
        }

        MlpPredictor m(static_cast<std::size_t>(hidden));
        m.w1_ = detail::get_f32_array(is, kMaxHiddenUnits * kFeatureCount);
        m.b1_ = detail::get_f32_array(is, kMaxHiddenUnits);
        m.w2_ = detail::get_f32_array(is, kMaxHiddenUnits);
        m.b2_ = detail::get_f32(is);

        if (m.w1_.size() != m.hidden_units_ * kFeatureCount || m.b1_.size() != m.hidden_units_ ||
            m.w2_.size() != m.hidden_units_) {
            throw ModelSerializationError(
                "MLP parameter arrays are inconsistent with the declared hidden width");
        }

        m.calibrator_.set_temperature(detail::get_f32(is));
        m.config_.learning_rate    = detail::get_f32(is);
        m.config_.l2               = detail::get_f32(is);
        m.config_.positive_weight  = detail::get_f32(is);
        m.config_.max_weight_delta = detail::get_f32(is);
        m.config_.validate();
        m.updates_ = detail::get_u64(is);

        for (float w : m.w1_) {
            if (!std::isfinite(w)) {
                throw ModelSerializationError("MLP file contains a non-finite hidden weight");
            }
        }
        for (float w : m.w2_) {
            if (!std::isfinite(w)) {
                throw ModelSerializationError("MLP file contains a non-finite output weight");
            }
        }
        return m;
    }

private:
    void resize(std::size_t hidden_units) {
        if (hidden_units == 0 || hidden_units > kMaxHiddenUnits) {
            throw InvalidConfigError("hidden layer must have between 1 and " +
                                     std::to_string(kMaxHiddenUnits) + " units, got " +
                                     std::to_string(hidden_units));
        }
        hidden_units_ = hidden_units;
        w1_.assign(hidden_units * kFeatureCount, 0.0F);
        b1_.assign(hidden_units, 0.0F);
        w2_.assign(hidden_units, 0.0F);
        b2_ = 0.0F;
    }

    /// Forward pass. Writes the hidden activations, returns the output logit.
    ///
    /// Accumulators are double for the same determinism reason as the linear
    /// model's dot product.
    [[nodiscard]] float forward(const FeatureVector& x,
                                std::array<float, kMaxHiddenUnits>& hidden) const noexcept {
        double out = static_cast<double>(b2_);
        for (std::size_t j = 0; j < hidden_units_; ++j) {
            const float* row = &w1_[j * kFeatureCount];
            double       acc = static_cast<double>(b1_[j]);
            for (std::size_t i = 0; i < kFeatureCount; ++i) {
                acc += static_cast<double>(row[i]) * static_cast<double>(x[i]);
            }
            const float a = acc > 0.0 ? static_cast<float>(acc) : 0.0F;  // ReLU
            hidden[j]     = a;
            out += static_cast<double>(w2_[j]) * static_cast<double>(a);
        }
        return static_cast<float>(out);
    }

    [[nodiscard]] float example_weight(const TrainingExample& example) const noexcept {
        const float base =
            std::isfinite(example.weight) && example.weight > 0.0F ? example.weight : 1.0F;
        return example.target > 0.5F ? base * config_.positive_weight : base;
    }

    std::size_t          hidden_units_ = 0;
    std::vector<float>   w1_;  ///< row-major, hidden_units_ x kFeatureCount
    std::vector<float>   b1_;
    std::vector<float>   w2_;
    float                b2_ = 0.0F;
    LinearTrainingConfig config_{};
    TemperatureScaler    calibrator_{};
    std::uint64_t        updates_ = 0;
};

}  // namespace lpq

#endif  // LPQ_MODELS_MLP_PREDICTOR_HPP
