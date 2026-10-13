// =============================================================================
// Correction engine (ESD.md section 10, ESD.md section 16, PRD.md section 13).
//
// ESD.md section 16 specifies the failure path exactly:
//
//     prediction failure
//         -> expand candidate set
//         -> re-evaluate
//         -> if still uncertain
//         -> authoritative classical extraction
//
//     Every correction must be recorded.
//
// This class is that loop, and nothing else. Verification decides whether a
// candidate is extractable; this decides what to do when it is not.
//
// -----------------------------------------------------------------------------
// Why the loop needs a callback rather than owning the queue
// -----------------------------------------------------------------------------
//
// Expanding the window is useless unless the wider window is then FILLED, and
// only the LPQ can fill it -- it is the only component that knows which
// authoritative substrate is installed and how to obtain more candidates from
// it. So the caller passes a refill function.
//
// The alternative, giving this class a queue reference, would drag substrate
// knowledge into the correction policy and make the policy untestable without a
// queue. As written, every test below drives the full failure path with a
// closure over a plain vector.
//
// -----------------------------------------------------------------------------
// The four repair mechanisms in ESD.md section 10
// -----------------------------------------------------------------------------
//
// ESD.md section 10 lists four ways to repair an incorrect learned ordering.
// Three are implemented here; one is deliberately not, and it is worth being
// explicit about which is which.
//
//   EXPANDING THE CANDIDATE WINDOW -- implemented, the primary mechanism.
//
//   REORDERING -- implemented, though it does not look like a separate step.
//     When verification accepts a candidate other than the model's top pick,
//     the ordering HAS been repaired: the correct node is extracted and the
//     mistake is recorded as a correction. No explicit re-sort is needed
//     because the window is already key-comparable.
//
//   DELEGATING TO THE AUTHORITATIVE QUEUE -- implemented, as the terminal
//     fallback once the expansion or verification budget is spent.
//
//   REINSERTING -- NOT implemented here, because whether it is needed at all
//     depends on how the LPQ sources candidates, which is a property of the
//     substrate rather than of correction policy. A substrate that can expose
//     candidates without removing them has nothing to reinsert; one that pops
//     k entries to form a window must return the k-1 it did not extract.
//     Putting reinsertion in this class would bake in the second assumption
//     and silently corrupt the first -- reinserting an element that was never
//     removed duplicates it, and a duplicate in the queue is a node settled
//     twice, which is a wrong answer rather than a slow one. It therefore
//     belongs to the LPQ, next to the code that did the removing.
// =============================================================================
#ifndef LPQ_VERIFICATION_CORRECTION_ENGINE_HPP
#define LPQ_VERIFICATION_CORRECTION_ENGINE_HPP

#include <concepts>
#include <cstdint>
#include <string_view>

#include "lpq/common/types.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/verification/verification_engine.hpp"

namespace lpq {

/// How an extraction was ultimately resolved.
enum class ResolutionPath : std::uint8_t {
    /// Verified on the first attempt with the model's top pick. The fast path.
    kFirstAttempt = 0,

    /// Verified on the first attempt, but the accepted candidate was not the
    /// model's first choice. Still cheap -- no extra queue work -- but the
    /// prediction was wrong and is recorded as such.
    kFirstAttemptCorrected,

    /// Verified only after the window was widened at least once.
    kExpanded,

    /// Never verified. The caller must extract from the authoritative queue.
    kFallback,

    /// The window was empty, so there was nothing to attempt.
    kEmpty,
};

[[nodiscard]] inline constexpr std::string_view resolution_path_name(ResolutionPath p) noexcept {
    switch (p) {
        case ResolutionPath::kFirstAttempt:          return "first_attempt";
        case ResolutionPath::kFirstAttemptCorrected: return "first_attempt_corrected";
        case ResolutionPath::kExpanded:              return "expanded";
        case ResolutionPath::kFallback:              return "fallback";
        case ResolutionPath::kEmpty:                 return "empty";
    }
    return "unknown";
}

/// The result of driving one extraction to a conclusion.
struct CorrectionOutcome {
    ResolutionPath path = ResolutionPath::kEmpty;

    /// The verification that succeeded. Only meaningful when `resolved` is
    /// true; on a fallback it holds the last failed attempt, which is what the
    /// learner needs in order to know how badly the prediction missed.
    VerificationResult verification{};

    std::uint32_t attempts   = 0;  ///< verifications performed
    std::uint32_t expansions = 0;  ///< window widenings
    std::uint32_t comparisons = 0; ///< total verification comparisons

    // --- Identity of the decision, stamped by the queue (D-12 / M5-O2) -------------------------
    //
    // `verification.node` is the node the verifier ACCEPTED; nothing recorded which node the model
    // ranked first, so a wrongly ranked first choice could not be turned into a labelled example.
    // The queue fills these in just before it reports the extraction. kInvalidNode means "not
    // provided": the controller-retreat path makes no prediction and leaves them unset.

    /// The model's first choice in the window the verifier last examined (the accepted window when
    /// the extraction resolved, the last failed one on a fallback).
    NodeId predicted_node = kInvalidNode;
    double predicted_key  = 0.0;

    /// The node actually returned by the extraction, with its key. Always the authoritative
    /// answer, including on a fallback, where `verification.node` is only the last failed attempt.
    NodeId extracted_node = kInvalidNode;
    double extracted_key  = 0.0;

    /// True when a prediction was made and its first choice was itself a minimum: its key equals
    /// the extracted key. Decided on keys rather than node identity so that a tie is not punished.
    [[nodiscard]] bool first_choice_was_minimum() const noexcept {
        return predicted_node != kInvalidNode && predicted_key <= extracted_key;
    }

    /// True when a candidate was proven extractable. False means the caller
    /// must fall back to the authoritative queue.
    [[nodiscard]] bool resolved() const noexcept {
        return path == ResolutionPath::kFirstAttempt ||
               path == ResolutionPath::kFirstAttemptCorrected ||
               path == ResolutionPath::kExpanded;
    }

    /// True when the extraction cost more than the ideal single verification.
    /// This is the quantity a learned queue has to keep small to be worth
    /// running at all.
    [[nodiscard]] bool was_expensive() const noexcept {
        return path == ResolutionPath::kExpanded || path == ResolutionPath::kFallback;
    }
};

/// Counters for PRD.md section 13 ("number of corrections", "number of fallback
/// operations") and ESD.md section 22.
struct CorrectionMetrics {
    std::uint64_t extractions              = 0;
    std::uint64_t first_attempt            = 0;
    std::uint64_t first_attempt_corrected  = 0;
    std::uint64_t expanded                 = 0;
    std::uint64_t fallbacks                = 0;
    std::uint64_t empty                    = 0;

    std::uint64_t total_attempts           = 0;
    std::uint64_t total_expansions         = 0;
    std::uint64_t total_comparisons        = 0;

    /// Fraction of extractions that took the ideal path: verified immediately,
    /// with the model's first choice.
    ///
    /// This is the headline number for prediction usefulness. It is NOT the
    /// same as the verification engine's prediction accuracy, which is
    /// conditioned on a verification having succeeded; this one counts
    /// fallbacks against the model, as it should, because a fallback is a
    /// prediction that cost time and delivered nothing.
    [[nodiscard]] double fast_path_rate() const noexcept {
        return extractions == 0 ? 0.0
                                : static_cast<double>(first_attempt) /
                                      static_cast<double>(extractions);
    }

    [[nodiscard]] double fallback_rate() const noexcept {
        return extractions == 0
                   ? 0.0
                   : static_cast<double>(fallbacks) / static_cast<double>(extractions);
    }

    [[nodiscard]] double mean_attempts_per_extraction() const noexcept {
        return extractions == 0 ? 0.0
                                : static_cast<double>(total_attempts) /
                                      static_cast<double>(extractions);
    }

    void reset() noexcept { *this = CorrectionMetrics{}; }
};

/// What the correction loop requires of a refill function.
///
/// Called after each expansion with the widened manager. It repopulates the
/// window from the authoritative substrate and returns the bound that now
/// applies to whatever it did not offer.
///
/// Returning a fresh bound each time is required rather than convenient: a
/// wider window covers more of the queue, so the bound on the remainder
/// legitimately rises, and reusing the original bound would make expansion
/// pointless -- the test would be no easier to pass than before.
///
/// The refill must offer only entries it has NOT already offered during this
/// extraction. expand() keeps what was admitted, so a refill that restarted its
/// enumeration would re-offer the same high-scoring entries and the widened
/// window would fill with duplicates rather than reaching further into the
/// queue. Uniqueness is on the (node, key) entry rather than on the node,
/// because lazy deletion legitimately queues a node more than once. See
/// CandidateManager::offer, which asserts this in Debug builds.
template <typename F>
concept CandidateRefill = requires(F f, CandidateManager& manager) {
    { f(manager) } -> std::same_as<QueueBound>;
};

/// Drives one extraction through verify / expand / retry / fall back.
class CorrectionEngine {
public:
    CorrectionEngine() = default;

    /// Resolves one extraction.
    ///
    /// Preconditions: `manager` has had begin_extraction() called and has been
    /// populated once; `bound` describes the queue outside that initial window.
    ///
    /// On a fallback the manager is left as it stands, so the caller can still
    /// inspect what was considered -- useful for the learner, which wants to
    /// know what the model offered even when none of it could be used.
    template <CandidateRefill Refill>
    CorrectionOutcome resolve(CandidateManager& manager, VerificationEngine& verifier,
                              QueueBound bound, Refill&& refill) {
        CorrectionOutcome outcome;
        ++metrics_.extractions;

        if (manager.empty()) {
            outcome.path = ResolutionPath::kEmpty;
            ++metrics_.empty;
            return outcome;
        }

        for (;;) {
            const VerificationResult r =
                verifier.verify(manager.candidates(), bound, outcome.comparisons);

            ++outcome.attempts;
            outcome.comparisons += r.comparisons;
            outcome.verification = r;

            if (r.accepted()) {
                if (outcome.expansions > 0) {
                    outcome.path = ResolutionPath::kExpanded;
                    ++metrics_.expanded;
                } else if (r.prediction_was_correct) {
                    outcome.path = ResolutionPath::kFirstAttempt;
                    ++metrics_.first_attempt;
                } else {
                    outcome.path = ResolutionPath::kFirstAttemptCorrected;
                    ++metrics_.first_attempt_corrected;
                }
                break;
            }

            // Budget exhaustion is terminal. Unlike a narrow window it cannot
            // be repaired by looking at more candidates, because the cost of
            // looking is exactly what ran out.
            if (r.outcome == VerificationOutcome::kBudgetExhausted || !manager.expand()) {
                outcome.path = ResolutionPath::kFallback;
                ++metrics_.fallbacks;
                break;
            }

            ++outcome.expansions;
            bound = refill(manager);
        }

        metrics_.total_attempts += outcome.attempts;
        metrics_.total_expansions += outcome.expansions;
        metrics_.total_comparisons += outcome.comparisons;
        return outcome;
    }

    [[nodiscard]] const CorrectionMetrics& metrics() const noexcept { return metrics_; }
    void                                   reset_metrics() noexcept { metrics_.reset(); }

private:
    CorrectionMetrics metrics_{};
};

}  // namespace lpq

#endif  // LPQ_VERIFICATION_CORRECTION_ENGINE_HPP
