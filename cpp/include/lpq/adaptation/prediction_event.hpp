// =============================================================================
// Prediction events (ESD.md section 14, PRD.md section 11).
//
// ESD.md section 14: "Every verified queue decision produces an event", with
// the fields reproduced below. This is that record, and it is the only channel
// through which the learner hears about the world.
//
// -----------------------------------------------------------------------------
// Why the label is trustworthy
// -----------------------------------------------------------------------------
//
// Every field describing the outcome -- actual_rank, rank_error, was_correct --
// comes from the VERIFICATION engine, which derives it from authoritative keys
// rather than from any model output. So the training signal is correct by
// construction: the learner cannot be taught to agree with a mistake, because
// the labels never pass through the thing that made the mistake.
//
// That property is what makes online learning safe here at all. A learner fed
// its own predictions would drift without bound and nothing downstream would
// notice, since the search would still return correct answers -- just slower
// and slower.
//
// -----------------------------------------------------------------------------
// The cost of carrying features
// -----------------------------------------------------------------------------
//
// An event carries the feature vector it was scored from, because an update
// applied later cannot reconstruct it: the search has moved on and the dynamic
// features no longer hold the values they had. At 28 floats that is 112 bytes
// per event, which bounds how many events can be buffered and is why the ring
// buffer in online_learner.hpp has a configurable, enforced capacity rather
// than growing.
// =============================================================================
#ifndef LPQ_ADAPTATION_PREDICTION_EVENT_HPP
#define LPQ_ADAPTATION_PREDICTION_EVENT_HPP

#include <cstddef>
#include <cstdint>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/prediction.hpp"
#include "lpq/verification/verification_engine.hpp"

namespace lpq {

/// Where the learner gets the feature vector of a node (M5-O2).
///
/// The queue reports node ids only; it never sees features. The component that scores pushes also
/// implements this, so a training example is built from exactly the vector the model was scored
/// with. Static features only: the dynamic groups are zero in a per-node score by construction.
class FeatureSource {
public:
    virtual ~FeatureSource() = default;

    /// Fills `out` with the features the scorer uses for node `u`.
    virtual void features_of(NodeId u, FeatureVector& out) const noexcept = 0;
};

/// One verified queue decision, exactly as ESD.md section 14 specifies.
///
/// Version 2 (M5-O2) adds the accepted node's features, so that a wrong first choice can be turned
/// into a negative example for it and a positive one for the node that was actually extracted.
struct PredictionEvent {
    /// The features the prediction was made from. Captured at prediction time,
    /// because they cannot be recovered afterwards.
    FeatureVector features{};

    /// The node the model ranked first.
    NodeId predicted_node = 0;

    /// The rank the model assigned, or kUnknownRank for a pointwise model.
    std::uint32_t predicted_rank = kUnknownRank;

    /// The model's calibrated confidence in that choice.
    float confidence = 0.0F;

    /// Where the node that was actually extractable sat in the model's
    /// ordering. Zero means the model's first choice was right.
    std::uint32_t actual_rank = 0;

    /// actual_rank - predicted_rank where a rank was predicted; otherwise
    /// actual_rank, which is the displacement from the model's first choice.
    std::uint32_t rank_error = 0;

    bool was_correct = false;

    /// Verification comparisons this decision cost. Lets the learner weigh an
    /// expensive mistake more heavily than a cheap one.
    std::uint32_t verification_cost = 0;

    /// The node the verifier accepted (the authoritative answer) and its features. Set only when the
    /// model's first choice was NOT a minimum: the event then yields two examples, a negative for
    /// the first choice and a positive for this node, both from the verifier and neither from a
    /// model output. When the first choice was right this is unset and `features` is the positive.
    FeatureVector accepted_features{};
    NodeId        accepted_node         = kInvalidNode;
    bool          has_accepted_example  = false;

    /// Whether the decision ended in a fallback to the authoritative queue.
    ///
    /// Distinct from !was_correct: a wrong first choice that verification
    /// repaired within the window is cheap, while a fallback threw away the
    /// whole prediction and paid the classical cost on top.
    bool fallback_used = false;

    /// Builds an event from a verification outcome.
    ///
    /// `features` must be the vector the prediction was made from.
    [[nodiscard]] static PredictionEvent from_verification(const FeatureVector&      features,
                                                           NodeId                    predicted,
                                                           float                     confidence,
                                                           const VerificationResult& result,
                                                           bool fell_back) {
        PredictionEvent e;
        e.features          = features;
        e.predicted_node    = predicted;
        e.confidence        = confidence;
        e.actual_rank       = result.rank_displacement;
        e.rank_error        = result.rank_displacement;
        e.was_correct       = result.prediction_was_correct;
        e.verification_cost = result.comparisons;
        e.fallback_used     = fell_back;
        return e;
    }

    /// The supervised example this event implies for the model that scored it.
    ///
    /// The target is 1 when the model's first choice was right and 0 otherwise,
    /// which is the extraction-likelihood formulation of PRD.md section 8.
    ///
    /// The weight scales with what the mistake cost. A wrong choice that
    /// verification repaired inside the window is a mild error; one that forced
    /// a fallback discarded the prediction entirely and paid the classical cost
    /// as well, so it carries more weight. Treating those alike would train the
    /// model to avoid cheap mistakes as hard as expensive ones.
    [[nodiscard]] TrainingExample to_example(float fallback_penalty = 4.0F) const {
        TrainingExample ex;
        ex.features = features;
        ex.target   = was_correct ? 1.0F : 0.0F;
        // The penalty marks a first choice that was wrong AND forced a fallback. A first choice
        // that was a minimum keeps weight 1 even when verification could not certify it.
        ex.weight   = (fallback_used && !was_correct) ? fallback_penalty : 1.0F;
        return ex;
    }

    /// The positive example for the accepted node, when the first choice was wrong.
    [[nodiscard]] TrainingExample accepted_example() const {
        TrainingExample ex;
        ex.features = accepted_features;
        ex.target   = 1.0F;
        ex.weight   = 1.0F;
        return ex;
    }

    /// Training examples this event carries (0, 1 or 2).
    [[nodiscard]] std::size_t example_count() const noexcept {
        return has_accepted_example ? 2U : 1U;
    }
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_PREDICTION_EVENT_HPP
