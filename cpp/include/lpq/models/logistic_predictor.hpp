// =============================================================================
// Online logistic predictor (ESD.md section 12: "Online logistic/perceptron
// predictor", PRD.md section 10).
//
//     P(u is the correct next extraction | x) = sigmoid(w . x + b)
//
// This is the production candidate for the learned path. It is the classifier
// formulation of PRD.md section 8's primary target -- extraction likelihood --
// and its inference cost is identical to the linear model's plus one
// exponential.
//
// -----------------------------------------------------------------------------
// Logistic rather than perceptron
// -----------------------------------------------------------------------------
//
// ESD.md section 12 offers the choice. Logistic is implemented and the
// perceptron is not, for one decisive reason: PRD.md section 12 requires the
// predictor to expose a CALIBRATED confidence, and the adaptation controller
// (PRD.md section 14) gates on it.
//
// The perceptron's hinge-loss update produces a decision boundary and a signed
// margin, but the margin is not a probability and has no scale that survives
// across graphs -- a margin of 2.0 means something different on every workload.
// There is nothing to calibrate, so the controller would be thresholding on an
// arbitrary quantity. Log-loss produces a probability directly, which is what
// temperature scaling can then correct.
//
// The perceptron is the hinge-loss special case of this same weight vector and
// could be added as an ablation if the log-loss formulation underperforms; the
// parameter block and serialization would be unchanged.
//
// -----------------------------------------------------------------------------
// Why the gradient is so simple
// -----------------------------------------------------------------------------
//
// For log-loss with a sigmoid link, d(loss)/d(w_i) = (sigmoid(z) - y) * x_i.
// The sigmoid's derivative cancels against the log-loss denominator exactly.
// That is worth noting because it means this model's update costs the same as
// the squared-loss one -- no extra term, no second exponential -- which keeps
// PRD.md section 11's "bounded update cost" requirement easy to honour.
//
// -----------------------------------------------------------------------------
// Score versus confidence
// -----------------------------------------------------------------------------
//
// predict() returns the LOGIT as the score and the calibrated sigmoid as the
// confidence. The logit is used for ordering because it is monotone in the
// probability and avoids the sigmoid's saturation: two strongly positive
// candidates can both round to a confidence of 1.0 in float while their logits
// remain clearly ordered. Ranking on confidence would make those candidates
// tie, and the tie-break would then decide the search path rather than the
// model.
// =============================================================================
#ifndef LPQ_MODELS_LOGISTIC_PREDICTOR_HPP
#define LPQ_MODELS_LOGISTIC_PREDICTOR_HPP

#include <cmath>
#include <istream>
#include <ostream>
#include <string_view>

#include "lpq/models/calibration.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// Online logistic regression over the feature vector.
///
/// Derives from LinearPredictor to share the parameter block, its
/// serialization and the clipped, decayed gradient step -- the two models
/// differ only in their loss. The class is final and LinearPredictor has no
/// virtual functions, so no base-pointer deletion is possible; this is code
/// reuse, not a polymorphic hierarchy. Runtime polymorphism is provided
/// separately by PredictorAdapter.
class LogisticPredictor final : public LinearPredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kLogistic;
    static constexpr std::string_view kName = "logistic";

    LogisticPredictor() = default;

    explicit LogisticPredictor(LinearTrainingConfig config) : LinearPredictor(config) {}

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        const float z = detail::dot(weights_, features) + bias_;

        Prediction p;
        p.score          = z;  // the logit: see the header
        p.confidence     = calibrator_.calibrate_logit(z);
        p.uncertainty    = uncertainty_from_confidence(p.confidence);
        p.estimated_rank = kUnknownRank;
        return p;
    }

    /// One log-loss SGD step.
    void update(const TrainingExample& example) noexcept {
        const float z = detail::dot(weights_, example.features) + bias_;
        if (!std::isfinite(z)) {
            return;
        }

        // The UNCALIBRATED probability is used for the gradient. The calibrator
        // corrects the model's reported confidence after the fact; feeding the
        // corrected value back into training would make the model chase its own
        // correction, and the two would no longer converge to anything
        // meaningful.
        const float prediction = logistic(z);
        const float error      = prediction - example.target;

        apply_gradient(example.features, error * example_weight(example));
    }

    [[nodiscard]] bool supports_online_update() const noexcept { return true; }

    /// Probability this candidate is the correct next extraction, uncalibrated.
    [[nodiscard]] float probability(const FeatureVector& features) const noexcept {
        return logistic(detail::dot(weights_, features) + bias_);
    }

    void save(std::ostream& os) const { save_as(os, kKind); }

    [[nodiscard]] static LogisticPredictor load(std::istream& is) {
        LogisticPredictor m;
        m.load_body(is, read_model_header_expecting(is, kKind));
        return m;
    }
};

}  // namespace lpq

#endif  // LPQ_MODELS_LOGISTIC_PREDICTOR_HPP
