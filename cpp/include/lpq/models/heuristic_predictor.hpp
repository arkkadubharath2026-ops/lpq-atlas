// =============================================================================
// Baseline predictors (PRD.md section 10: "Constant/heuristic baseline",
// ESD.md section 12: "Heuristic baseline").
//
// Two baselines, because they answer different questions.
//
// -----------------------------------------------------------------------------
// ConstantPredictor -- the null model
// -----------------------------------------------------------------------------
//
// Returns the same score for every candidate, so it induces no ordering at all.
// Its purpose is to be the floor: any measurement of "learned ordering helps"
// must beat a predictor that supplies no ordering information whatsoever. It
// also isolates the cost of the surrounding machinery -- feature extraction,
// candidate management, verification -- from the cost of the model, since
// running the full LPQ pipeline with this model measures everything except
// inference and everything except useful prediction.
//
// -----------------------------------------------------------------------------
// KeyHeuristicPredictor -- and why it is a HARD baseline, not a weak one
// -----------------------------------------------------------------------------
//
// This model scores a candidate by the negation of its effective key. It
// contains no learned parameters and it is, for exact Dijkstra, a PERFECT
// predictor of which candidate should be extracted next: the correct next
// extraction is by definition the one with the smallest key, and the key is a
// feature.
//
// That is not a flaw in the baseline. It is the single most important fact
// about this project's research question, and stating it plainly here is more
// useful than discovering it later in an experiment:
//
//     Predicting WHICH candidate is the minimum is trivial when the keys are
//     known. The expensive thing a priority queue does is not identifying the
//     minimum -- it is MAINTAINING the order that makes the minimum
//     identifiable in constant time, across every insertion and decrease-key.
//
// So a learned predictor cannot justify itself by being accurate. An accurate
// predictor that still has to be handed every candidate's exact key has saved
// nothing, because obtaining those keys is not the cost. It can only justify
// itself by letting the queue do LESS ORDERING WORK -- maintaining a weaker,
// cheaper structure and using predictions to find the minimum within it -- and
// the measurement that matters is end-to-end time against the classical queues,
// not prediction accuracy.
//
// Keeping this baseline in the comparison set enforces that honesty. If a
// learned model reports better accuracy than KeyHeuristicPredictor, something
// is wrong with the evaluation, because this baseline is already at the
// accuracy ceiling. If it reports better end-to-end runtime, that is a real
// result.
// =============================================================================
#ifndef LPQ_MODELS_HEURISTIC_PREDICTOR_HPP
#define LPQ_MODELS_HEURISTIC_PREDICTOR_HPP

#include <cmath>
#include <istream>
#include <ostream>
#include <string_view>

#include "lpq/features/feature_vector.hpp"
#include "lpq/models/calibration.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {

/// Scores every candidate identically. The null model.
class ConstantPredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kHeuristic;
    static constexpr std::string_view kName = "constant";

    ConstantPredictor() = default;

    explicit ConstantPredictor(float score, float confidence = 0.5F)
        : score_(score), confidence_(confidence) {}

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        (void)features;  // deliberately ignored; that is the point of this model
        Prediction p;
        p.score      = score_;
        p.confidence = confidence_;
        // Maximal uncertainty, always. This model has no basis for preferring
        // one candidate over another, and reporting anything else would let the
        // adaptation controller act on a certainty that does not exist.
        p.uncertainty    = 1.0F;
        p.estimated_rank = kUnknownRank;
        return p;
    }

    void update(const TrainingExample& example) noexcept { (void)example; }

    [[nodiscard]] bool supports_online_update() const noexcept { return false; }

    void save(std::ostream& os) const {
        write_model_header(os, kKind);
        detail::put_u32(os, 0U);  // sub-kind: constant
        detail::put_f32(os, score_);
        detail::put_f32(os, confidence_);
    }

    [[nodiscard]] static ConstantPredictor load(std::istream& is) {
        (void)read_model_header_expecting(is, kKind);
        const std::uint32_t sub = detail::get_u32(is);
        if (sub != 0U) {
            throw ModelSerializationError(
                "baseline model file is not a constant predictor");
        }
        ConstantPredictor m;
        m.score_      = detail::get_f32(is);
        m.confidence_ = detail::get_f32(is);
        return m;
    }

private:
    float score_      = 0.0F;
    float confidence_ = 0.5F;
};

/// Scores by the negated effective key. No learned parameters.
///
/// See the header for why this is the hard baseline rather than the easy one.
class KeyHeuristicPredictor {
public:
    static constexpr ModelKind        kKind = ModelKind::kHeuristic;
    static constexpr std::string_view kName = "key_heuristic";

    KeyHeuristicPredictor() = default;

    /// `gap_scale` controls how quickly confidence falls as a candidate's key
    /// rises above the current queue minimum.
    explicit KeyHeuristicPredictor(float gap_scale) : gap_scale_(gap_scale) {}

    [[nodiscard]] Prediction predict(const FeatureVector& features) const noexcept {
        Prediction p;

        // Negated, because the score convention is "higher is more likely to be
        // extracted next" while a key is "lower is sooner".
        p.score = -get_feature(features, Feature::kEffectiveKey);

        // Confidence from the normalised gap to the current queue minimum: a
        // candidate sitting exactly at the minimum is certainly next; one far
        // above it certainly is not. This uses only information the queue
        // already has, so the baseline stays parameter-free.
        //
        // The gap feature is clamped at zero by the extractor, so a negative
        // value cannot arrive here and the exponential cannot exceed 1.
        const float gap = get_feature(features, Feature::kKeyGapFromMinimum);
        p.confidence    = std::exp(-gap * gap_scale_);
        p.uncertainty   = uncertainty_from_confidence(p.confidence);
        p.estimated_rank = kUnknownRank;
        return p;
    }

    void update(const TrainingExample& example) noexcept { (void)example; }

    [[nodiscard]] bool supports_online_update() const noexcept { return false; }

    void save(std::ostream& os) const {
        write_model_header(os, kKind);
        detail::put_u32(os, 1U);  // sub-kind: key heuristic
        detail::put_f32(os, gap_scale_);
    }

    [[nodiscard]] static KeyHeuristicPredictor load(std::istream& is) {
        (void)read_model_header_expecting(is, kKind);
        const std::uint32_t sub = detail::get_u32(is);
        if (sub != 1U) {
            throw ModelSerializationError(
                "baseline model file is not a key-heuristic predictor");
        }
        KeyHeuristicPredictor m;
        m.gap_scale_ = detail::get_f32(is);
        return m;
    }

private:
    /// Default chosen so that a candidate one full normalised unit above the
    /// minimum reports a confidence near e^-1. It is a shape parameter of a
    /// baseline, not a tuned hyperparameter, and no result depends on it.
    float gap_scale_ = 1.0F;
};

}  // namespace lpq

#endif  // LPQ_MODELS_HEURISTIC_PREDICTOR_HPP
