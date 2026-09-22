// =============================================================================
// Prediction and training-example types (ESD.md section 10, PRD.md section 8).
//
// ESD.md section 10 specifies the shape of a prediction exactly:
//
//     Prediction { node, score, confidence, estimated_rank, uncertainty }
//
// -----------------------------------------------------------------------------
// What a score means
// -----------------------------------------------------------------------------
//
// PRD.md section 8: "The preferred primary target is relative ordering /
// extraction likelihood, not raw distance prediction." A score is therefore an
// ORDERING signal, not an estimate of any distance. Higher means "more likely
// to be the correct next extraction". Only the induced ordering is meaningful:
// two models may produce scores on completely different scales and still be
// equivalent predictors, which is why every consumer compares scores and none
// of them interprets a magnitude.
//
// This is what makes the safety argument work. A score cannot be "wrong" in a
// way that breaks correctness, because nothing downstream treats it as a
// distance -- the verification layer re-derives correctness from the
// authoritative queue regardless of what the score said (PRD.md section 4).
//
// -----------------------------------------------------------------------------
// Why estimated_rank is usually unknown
// -----------------------------------------------------------------------------
//
// A pointwise predictor sees one candidate's features and cannot know where
// that candidate falls among candidates it has not been shown. Inventing a rank
// from a single score -- by mapping confidence onto queue size, say -- would
// manufacture a number that looks like a measurement and is not one, and the
// adaptation controller would then be tuning against an artefact.
//
// So pointwise models report kUnknownRank, and rank is computed by the
// CandidateManager, which does see the whole candidate set and can order it.
// The field stays in the struct because a genuinely rank-predicting model (a
// pairwise or listwise formulation, PRD.md section 8 item 1) can fill it
// honestly, and consumers must handle both.
// =============================================================================
#ifndef LPQ_MODELS_PREDICTION_HPP
#define LPQ_MODELS_PREDICTION_HPP

#include <cstdint>
#include <limits>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"

namespace lpq {

/// Sentinel for "this model does not estimate rank". See the header.
inline constexpr std::uint32_t kUnknownRank = std::numeric_limits<std::uint32_t>::max();

/// One model output for one candidate node.
struct Prediction {
    NodeId node = 0;

    /// Ordering signal. Higher means more likely to be the correct next
    /// extraction. Scale is model-specific and carries no units.
    float score = 0.0F;

    /// Calibrated probability in [0, 1] that this candidate is the correct next
    /// extraction. Distinct from `score`: score orders, confidence gates. The
    /// adaptation controller reads confidence and never reads score, because a
    /// threshold on an uncalibrated score means something different for every
    /// model (PRD.md section 12).
    float confidence = 0.0F;

    /// Position this candidate is predicted to occupy, or kUnknownRank.
    std::uint32_t estimated_rank = kUnknownRank;

    /// How little the model commits to its own answer, in [0, 1]. Zero is a
    /// decisive prediction; one is maximal indecision. Derived from confidence
    /// for probabilistic models as the distance from a decisive 0 or 1.
    float uncertainty = 1.0F;

    [[nodiscard]] bool has_rank() const noexcept { return estimated_rank != kUnknownRank; }
};

/// Orders two predictions best-first. Used by the candidate manager.
///
/// Ties break on node id, not on the order predictions happened to arrive in.
/// An unstable tie-break would make the search path depend on candidate
/// enumeration order, and a run would stop being reproducible from its seed --
/// which would violate the reproducibility requirement in PRD.md section 24 in
/// a way that is very hard to notice, because the results would still be
/// correct, just not identical between runs.
struct PredictionBetter {
    [[nodiscard]] bool operator()(const Prediction& a, const Prediction& b) const noexcept {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.node < b.node;
    }
};

/// One labelled observation for training or online update.
///
/// The label is the verified outcome of a real queue decision (PRD.md section
/// 11: "Whether predicted candidate was actually minimum"), so it is generated
/// by the verification layer rather than by any model. That is what keeps
/// training grounded: the labels come from the authoritative queue, which is
/// correct by construction.
struct TrainingExample {
    FeatureVector features{};

    /// 1.0 if this candidate was in fact the correct next extraction, 0.0 if
    /// not. Held as a float rather than a bool so the same struct serves the
    /// regression ablation (PRD.md section 8: the raw-distance formulation),
    /// where the target is continuous.
    float target = 0.0F;

    /// Importance weight. Defaults to 1.
    ///
    /// Exists because the natural label distribution is extremely unbalanced:
    /// exactly one candidate per extraction is the true minimum and every other
    /// one is a negative, so a model minimising average loss can score well by
    /// answering "no" always. Weighting lets the online learner correct that
    /// without discarding observations.
    float weight = 1.0F;
};

}  // namespace lpq

#endif  // LPQ_MODELS_PREDICTION_HPP
