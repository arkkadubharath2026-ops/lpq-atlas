// =============================================================================
// Verification engine (ESD.md section 10, ESD.md section 16, PRD.md section 13).
//
// ESD.md section 10: "Determines whether the selected candidate satisfies the
// active correctness policy." PRD.md section 13 calls verification "a core
// research component", and it is the component that makes the whole design
// defensible: the predictor is allowed to be wrong because nothing it says can
// reach the answer without passing through here.
//
// -----------------------------------------------------------------------------
// The soundness argument, in full
// -----------------------------------------------------------------------------
//
// The obvious way to verify an extraction is to check the chosen node's key
// against every other key in the queue. That is exactly the O(n) scan a
// priority queue exists to avoid, so doing it per extraction would make the
// learned queue slower than the classical one it is trying to beat, and the
// entire project would be pointless.
//
// What makes a cheap check sound instead is a LOWER BOUND on everything not
// looked at. The caller supplies `rest_lower_bound`: a value no key outside the
// candidate window can fall below. Given that, with m = the smallest key inside
// the window:
//
//     if m <= rest_lower_bound, then m is the global minimum,
//     because every unexamined key is >= rest_lower_bound >= m.
//
// The check costs one comparison against a bound, plus a walk of at most k
// candidates to find m. No part of it depends on the prediction being any good.
//
// The bound is the caller's obligation, and a wrong bound is the one input that
// can break correctness. It is therefore never inferred here and never
// defaulted. What this engine can check locally, it does: a non-finite bound is
// rejected rather than used, because +infinity would accept every candidate
// unconditionally and an uninitialised or NaN bound would make the acceptance
// test meaningless.
//
// What it cannot check locally is a bound that is merely too permissive -- one
// that sits above a key actually sitting in the unexamined part of the queue.
// Nothing visible from inside this function distinguishes that from a correct
// bound. It is caught end to end instead: such a bound lets a non-minimal node
// be extracted, which in exact mode changes the returned path, which the
// differential test against the trusted reference detects. That is the right
// place for it, because it is the only place where the ground truth exists.
//
// Keys are non-negative throughout this project (they are distances under
// non-negative weights), which is what makes the multiplication by the slack
// factor monotone in the intended direction.
//
// -----------------------------------------------------------------------------
// What the prediction is actually for
// -----------------------------------------------------------------------------
//
// Note what the argument above does NOT use: the scores. Verification accepts
// the smallest-key candidate in the window, which is a fact about keys, not
// about the model.
//
// The model's job is to make the window CONTAIN that candidate while k stays
// small. A perfect predictor lets k be tiny; a useless one forces expansion or
// fallback and costs time. So prediction quality shows up as speed and never as
// correctness -- which is precisely the property PRD.md section 4 demands, and
// the reason a mispredicting model degrades gracefully instead of producing
// wrong answers.
//
// -----------------------------------------------------------------------------
// Approximate mode
// -----------------------------------------------------------------------------
//
// With an error budget the test relaxes to
//
//     accept c if c.key <= (1 + epsilon) * best_possible_key
//
// where best_possible_key = min(m, rest_lower_bound) is a valid lower bound on
// the true queue minimum. This yields a PROVEN per-extraction property: the
// extracted key is within (1 + epsilon) of the smallest key the queue could
// hold.
//
// It does NOT by itself prove the end-to-end contract in PRD.md section 4.2,
// that returned_cost <= (1 + epsilon) * optimal_cost. See
// correctness_policy.hpp for why the standard Dijkstra induction does not carry
// over unchanged. That is why bounded-approximate mode reports kEmpirical and
// why approximate runs measure the real ratio against the reference.
// =============================================================================
#ifndef LPQ_VERIFICATION_VERIFICATION_ENGINE_HPP
#define LPQ_VERIFICATION_VERIFICATION_ENGINE_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "lpq/common/types.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/verification/correctness_policy.hpp"

namespace lpq {

/// What verification concluded about one extraction attempt.
enum class VerificationOutcome : std::uint8_t {
    /// A candidate was proven extractable under the active policy.
    kAccepted = 0,

    /// No candidate in the window can be proven extractable, because something
    /// outside it may hold a smaller key. The caller should expand the window
    /// and retry (ESD.md section 16).
    kWindowTooNarrow,

    /// The window is empty, so there is nothing to verify.
    kEmptyWindow,

    /// The verification budget is spent. The caller must fall back to the
    /// authoritative queue.
    kBudgetExhausted,
};

[[nodiscard]] inline constexpr std::string_view verification_outcome_name(
    VerificationOutcome o) noexcept {
    switch (o) {
        case VerificationOutcome::kAccepted:        return "accepted";
        case VerificationOutcome::kWindowTooNarrow: return "window_too_narrow";
        case VerificationOutcome::kEmptyWindow:     return "empty_window";
        case VerificationOutcome::kBudgetExhausted: return "budget_exhausted";
    }
    return "unknown";
}

/// What the caller must know about the part of the queue it did not offer.
struct QueueBound {
    /// A value no key outside the candidate window falls below.
    ///
    /// The caller's obligation. If the window holds the entire queue, set
    /// `window_is_whole_queue` and this field is ignored.
    double rest_lower_bound = 0.0;

    /// True when every queued element was offered to the window, so there is
    /// nothing unexamined and no bound is needed.
    ///
    /// Not merely an optimisation: near the end of a search the queue is often
    /// smaller than k, and without this flag those extractions would need a
    /// bound that does not meaningfully exist.
    bool window_is_whole_queue = false;

    [[nodiscard]] static QueueBound whole_queue() noexcept {
        QueueBound b;
        b.window_is_whole_queue = true;
        // Negative infinity is the correct bound for "nothing outside": it can
        // never wrongly permit an acceptance, because the flag short-circuits
        // the comparison entirely, and if the flag were ever ignored the
        // conservative value would reject rather than accept.
        b.rest_lower_bound = -std::numeric_limits<double>::infinity();
        return b;
    }

    [[nodiscard]] static QueueBound at_least(double bound) noexcept {
        QueueBound b;
        b.rest_lower_bound = bound;
        return b;
    }
};

/// The outcome of one verification, plus the feedback the learner needs.
///
/// The displacement fields are what PRD.md section 11 requires as online
/// learning feedback: "whether predicted candidate was actually minimum",
/// "rank displacement", "absolute key displacement".
struct VerificationResult {
    VerificationOutcome outcome = VerificationOutcome::kEmptyWindow;

    /// The node proven extractable. Meaningful only when accepted.
    NodeId node = 0;
    double key  = 0.0;

    /// Whether the model's top-scored candidate was the one accepted.
    ///
    /// This is the label the online learner trains on, and it comes from the
    /// authoritative keys rather than from any model output -- which is what
    /// keeps training grounded in something correct by construction.
    bool prediction_was_correct = false;

    /// Where the accepted candidate sat in the model's ordering. Zero means
    /// the model's first choice was right.
    std::uint32_t rank_displacement = 0;

    /// key(model's first choice) - key(accepted). Never negative: the accepted
    /// candidate holds the smallest key in the window by construction.
    ///
    /// Carried alongside rank displacement because they measure different
    /// failures. Being wrong by one rank is cheap if the keys are nearly
    /// equal and expensive if they are far apart, and a controller tuned on
    /// rank alone could not tell those apart.
    double key_displacement = 0.0;

    /// Comparisons performed. The unit PRD.md section 13 asks to measure
    /// ("verification comparisons"), and machine-independent, unlike a time.
    std::uint32_t comparisons = 0;

    [[nodiscard]] bool accepted() const noexcept {
        return outcome == VerificationOutcome::kAccepted;
    }
};

/// Counters required by PRD.md section 13 and ESD.md section 22.
struct VerificationMetrics {
    std::uint64_t verifications          = 0;
    std::uint64_t accepted               = 0;
    std::uint64_t window_too_narrow      = 0;
    std::uint64_t budget_exhausted       = 0;
    std::uint64_t comparisons            = 0;

    /// Verifications refused because the caller's bound was not finite.
    /// Always zero in a correct run; non-zero means a caller bug, and it is
    /// counted rather than asserted so a long experiment reports it instead of
    /// dying halfway through.
    std::uint64_t rejected_invalid_bound = 0;

    /// Accepted, but the model's first choice was not the one accepted.
    /// ESD.md section 16: "Every correction must be recorded."
    std::uint64_t corrections             = 0;

    /// Accepted with the model's first choice, i.e. no correction needed.
    std::uint64_t predictions_correct     = 0;

    std::uint64_t rank_displacement_sum   = 0;
    double        key_displacement_sum    = 0.0;
    std::uint32_t max_rank_displacement   = 0;

    /// Fraction of accepted extractions where the model's top pick was right.
    ///
    /// Denominator is accepted verifications, not all verifications: an
    /// extraction that could not be verified has no ground truth to be right
    /// or wrong about, and counting it either way would distort the rate.
    [[nodiscard]] double prediction_accuracy() const noexcept {
        return accepted == 0 ? 0.0
                             : static_cast<double>(predictions_correct) /
                                   static_cast<double>(accepted);
    }

    [[nodiscard]] double correction_rate() const noexcept {
        return accepted == 0
                   ? 0.0
                   : static_cast<double>(corrections) / static_cast<double>(accepted);
    }

    [[nodiscard]] double mean_rank_displacement() const noexcept {
        return accepted == 0 ? 0.0
                             : static_cast<double>(rank_displacement_sum) /
                                   static_cast<double>(accepted);
    }

    [[nodiscard]] double mean_comparisons() const noexcept {
        return verifications == 0
                   ? 0.0
                   : static_cast<double>(comparisons) / static_cast<double>(verifications);
    }

    void reset() noexcept { *this = VerificationMetrics{}; }
};

/// Decides whether a candidate may be extracted under the active policy.
///
/// Holds no per-search state beyond its metrics, so one engine serves a whole
/// experiment and the counters accumulate across queries.
class VerificationEngine {
public:
    VerificationEngine() = default;

    explicit VerificationEngine(CorrectnessPolicy policy) : policy_(policy) {
        policy_.validate();
    }

    [[nodiscard]] const CorrectnessPolicy& policy() const noexcept { return policy_; }

    void set_policy(CorrectnessPolicy p) {
        p.validate();
        policy_ = p;
    }

    /// Verifies the candidate window against the bound on everything outside.
    ///
    /// `spent_budget` is the verification work already done for THIS
    /// extraction, across earlier attempts at narrower windows; it is the
    /// caller's running total, because the budget bounds the extraction rather
    /// than any single call.
    [[nodiscard]] VerificationResult verify(std::span<const Candidate> window,
                                            const QueueBound& bound,
                                            std::uint32_t     spent_budget = 0) noexcept {
        VerificationResult result;
        ++metrics_.verifications;

        if (window.empty()) {
            result.outcome = VerificationOutcome::kEmptyWindow;
            return result;
        }

        // A non-finite bound is refused rather than used. +infinity would
        // accept every candidate unconditionally, which is precisely the
        // failure this component exists to prevent; NaN would make every
        // comparison false and silently reject forever, which presents as an
        // unexplained fallback to the classical queue.
        if (!bound.window_is_whole_queue && !std::isfinite(bound.rest_lower_bound)) {
            result.outcome = VerificationOutcome::kWindowTooNarrow;
            ++metrics_.window_too_narrow;
            ++metrics_.rejected_invalid_bound;
            return result;
        }

        if (spent_budget >= policy_.verification_budget) {
            result.outcome = VerificationOutcome::kBudgetExhausted;
            ++metrics_.budget_exhausted;
            return result;
        }

        // --- Find the smallest key in the window -----------------------------
        //
        // Ties break on node id so the extraction order does not depend on the
        // order candidates happened to be offered in. Without it two runs from
        // the same seed could settle nodes in different orders and produce
        // different timings for reasons unrelated to the algorithm.
        std::size_t best_index = 0;
        for (std::size_t i = 1; i < window.size(); ++i) {
            ++result.comparisons;
            if (window[i].key < window[best_index].key ||
                (window[i].key == window[best_index].key &&
                 window[i].node < window[best_index].node)) {
                best_index = i;
            }
        }

        const Candidate& smallest = window[best_index];

        // --- Is it provably extractable? -------------------------------------
        //
        // In exact mode slack_factor() is exactly 1.0, so the multiplication is
        // an identity and the comparison is the plain m <= bound test.
        ++result.comparisons;
        bool acceptable = bound.window_is_whole_queue;
        if (!acceptable) {
            // The product is formed in double against a double bound. No
            // rounding slack is added: in exact mode any tolerance here would
            // be a licence to extract a node that is not the minimum, which is
            // the one error this component exists to prevent.
            acceptable = smallest.key <= policy_.slack_factor() * bound.rest_lower_bound;
        }

        if (!acceptable) {
            result.outcome = VerificationOutcome::kWindowTooNarrow;
            ++metrics_.window_too_narrow;
            metrics_.comparisons += result.comparisons;
            return result;
        }

        // --- Accepted ---------------------------------------------------------
        result.outcome = VerificationOutcome::kAccepted;
        result.node    = smallest.node;
        result.key     = smallest.key;

        result.prediction_was_correct = (best_index == 0);
        result.rank_displacement      = static_cast<std::uint32_t>(best_index);
        // window[0] is the model's first choice; the accepted candidate holds
        // the smallest key, so this difference cannot be negative.
        result.key_displacement = window[0].key - smallest.key;

        ++metrics_.accepted;
        metrics_.comparisons += result.comparisons;
        metrics_.rank_displacement_sum += result.rank_displacement;
        metrics_.key_displacement_sum += result.key_displacement;
        if (result.rank_displacement > metrics_.max_rank_displacement) {
            metrics_.max_rank_displacement = result.rank_displacement;
        }
        if (result.prediction_was_correct) {
            ++metrics_.predictions_correct;
        } else {
            ++metrics_.corrections;
        }
        return result;
    }

    /// Convenience overload taking the manager directly.
    [[nodiscard]] VerificationResult verify(const CandidateManager& manager,
                                            const QueueBound&       bound,
                                            std::uint32_t spent_budget = 0) noexcept {
        return verify(manager.candidates(), bound, spent_budget);
    }

    [[nodiscard]] const VerificationMetrics& metrics() const noexcept { return metrics_; }
    void                                     reset_metrics() noexcept { metrics_.reset(); }

private:
    CorrectnessPolicy   policy_{};
    VerificationMetrics metrics_{};
};

}  // namespace lpq

#endif  // LPQ_VERIFICATION_VERIFICATION_ENGINE_HPP
