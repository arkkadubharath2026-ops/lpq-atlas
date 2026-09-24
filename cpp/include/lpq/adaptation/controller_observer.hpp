// =============================================================================
// Connecting the adaptive controller and the online learner to a live queue
// (PRD.md sections 12-14, ESD.md section 39 milestones 20 and 21).
//
// Both components were implemented and unit-tested long before anything could
// run them: neither had a single caller outside its own tests, so "online
// updates work" was true in a test and untrue end to end. This is the adapter
// that closes that gap.
//
// It implements the queue's ExtractionObserver seam and does two things per
// extraction:
//
//   * feeds the controller what the extraction cost and achieved, so the
//     controller can decide whether the learned path is paying for itself;
//   * feeds the learner a PredictionEvent, so the model can be updated from
//     outcomes the verification layer certified.
//
// -----------------------------------------------------------------------------
// Why the labels are safe to learn from
// -----------------------------------------------------------------------------
//
// Every label here comes from the VERIFICATION layer, not from the model. The
// verifier establishes, against the authoritative bound, which candidate was
// actually extractable and how far the prediction sat from it. So the learner
// is corrected by ground truth produced during the search rather than by its
// own output, which is what stops online learning drifting onto its own
// predictions.
//
// This is also why ESD.md section 30 is not violated by online learning: the
// information used to update the model at extraction N is information the
// search legitimately had by extraction N. Nothing from a later query, and
// nothing from a test outcome, reaches the parameters.
// =============================================================================
#ifndef LPQ_ADAPTATION_CONTROLLER_OBSERVER_HPP
#define LPQ_ADAPTATION_CONTROLLER_OBSERVER_HPP

#include <cstdint>

#include "lpq/adaptation/adaptive_controller.hpp"
#include "lpq/adaptation/online_learner.hpp"
#include "lpq/adaptation/prediction_event.hpp"
#include "lpq/lpq/extraction_observer.hpp"

namespace lpq {

/// Drives an AdaptiveController, and optionally an OnlineLearner, from a queue.
///
/// Holds references rather than owning either, because both outlive a single
/// query while the queue is cleared between queries, and because a caller
/// measuring adaptation wants to read the controller's metrics afterwards.
class ControllerObserver final : public ExtractionObserver {
public:
    /// @param controller decides the policy. Required.
    /// @param learner    updated from verified outcomes. Optional: passing
    ///                   nullptr measures the controller alone, which is the
    ///                   ablation that separates adapting the POLICY from
    ///                   adapting the MODEL.
    explicit ControllerObserver(AdaptiveController& controller,
                                OnlineLearner* learner = nullptr) noexcept
        : controller_(controller), learner_(learner) {}

    [[nodiscard]] ExtractionPolicy policy() const noexcept override {
        const QueuePolicy p = controller_.policy();
        // A straight field-for-field copy. The two structs are deliberately
        // separate types -- see extraction_observer.hpp on why the queue cannot
        // import this layer's -- and a test asserts they stay in step.
        ExtractionPolicy out;
        out.use_learned_path = p.use_learned_path;
        out.window_size      = p.window_size;
        out.max_expansions   = p.max_expansions;
        return out;
    }

    void on_extraction(std::uint64_t entries_skipped,
                       std::uint32_t verification_comparisons,
                       const CorrectionOutcome& outcome) noexcept override {
        controller_.observe(entries_skipped, verification_comparisons, outcome);

        if (learner_ == nullptr) {
            return;
        }

        // The event carries what the verifier established, not what the model
        // claimed. `prediction_was_correct` is whether the highest-scoring
        // candidate was in fact the extractable one; `rank_displacement` is how
        // far down the window the truth actually sat.
        PredictionEvent event;
        event.predicted_node   = outcome.verification.node;
        event.was_correct      = outcome.verification.prediction_was_correct;
        event.actual_rank      = outcome.verification.rank_displacement;
        // rank_error is the distance between where the model ranked the true
        // minimum and where it actually was. With a per-node static scorer the
        // predicted rank is always the front of the window, so the error IS the
        // displacement; a model that ranked per instance would set both.
        event.rank_error       = outcome.verification.rank_displacement;
        event.verification_cost = verification_comparisons;
        event.fallback_used    = (outcome.path == ResolutionPath::kFallback);
        (void)entries_skipped;  // the controller uses it; the learner does not
        learner_->record(event);
    }

    [[nodiscard]] const AdaptiveController& controller() const noexcept {
        return controller_;
    }
    [[nodiscard]] const OnlineLearner* learner() const noexcept { return learner_; }

private:
    AdaptiveController& controller_;
    OnlineLearner*      learner_;
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_CONTROLLER_OBSERVER_HPP
