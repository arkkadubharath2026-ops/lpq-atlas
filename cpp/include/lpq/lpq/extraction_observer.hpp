// =============================================================================
// The seam between the learned queue and anything that wants to steer it
// (PRD.md sections 12-14, ESD.md section 40's "online updates work").
//
// The adaptive controller and the online learner both need two things from the
// queue: to be told what each extraction cost and achieved, and to be asked
// what policy the next extraction should use. Neither can be done by having the
// queue depend on `lpq::adaptation` -- adaptation already depends on
// verification, which depends on this layer, so the queue reaching the other
// way would close a cycle.
//
// So the queue depends on an INTERFACE declared here, and the adaptation layer
// implements it. The queue knows nothing about controllers, learners or modes;
// it knows that something may want to watch and may want to answer.
//
// -----------------------------------------------------------------------------
// The cost, stated rather than hidden
// -----------------------------------------------------------------------------
//
// A non-null observer costs one virtual call per extraction, and asking for a
// policy costs another. That is real, and it is why the pointer defaults to
// null and the control condition never sets one: a queue with no observer pays
// one predictable branch per pop and nothing else, so the adaptive
// configuration cannot silently tax the baseline it is compared against.
//
// This is the same reasoning that made the scorer a template parameter rather
// than a runtime flag. The difference is that a scorer is consulted on every
// push -- hundreds of millions of times -- while an observer is consulted once
// per extraction, and the adaptive mode is a distinct configuration that is
// measured separately rather than a variation the baseline pays for.
// =============================================================================
#ifndef LPQ_LPQ_EXTRACTION_OBSERVER_HPP
#define LPQ_LPQ_EXTRACTION_OBSERVER_HPP

#include <cstdint>

#include "lpq/verification/correction_engine.hpp"

namespace lpq {

/// What the queue should do for the next extraction.
///
/// Deliberately a copy of the fields `adaptation::QueuePolicy` carries rather
/// than that type itself: importing it would be the dependency this interface
/// exists to avoid. The adapter in the adaptation layer converts between them,
/// and a test asserts the two stay in step.
struct ExtractionPolicy {
    /// False sends the extraction straight to the classical bucket scan,
    /// bypassing the window and the verifier entirely. That is the controller's
    /// retreat position when prediction is not paying for itself.
    bool use_learned_path = true;

    /// How many entries the candidate window may hold.
    std::uint32_t window_size = 4;

    /// How many times the corrector may widen the window before giving up.
    std::uint32_t max_expansions = 2;
};

/// Watches extractions and, optionally, steers them.
///
/// Implementations live in the adaptation layer. The queue calls `policy()`
/// before building a window and `on_extraction()` after resolving one.
class ExtractionObserver {
public:
    virtual ~ExtractionObserver() = default;

    /// The policy for the extraction about to happen.
    ///
    /// Called once per pop, before any window is built. An implementation that
    /// does not steer returns the same value every time, which costs a virtual
    /// call and nothing else.
    [[nodiscard]] virtual ExtractionPolicy policy() const noexcept = 0;

    /// Reports one completed extraction.
    ///
    /// `entries_skipped` is how many queued entries the learned path avoided
    /// examining, and is zero on a fallback because a fallback examined all of
    /// them. `verification_comparisons` is what the checking cost. The outcome
    /// carries which resolution path was taken and, when the window resolved,
    /// how far the prediction was from the truth.
    ///
    /// Called after the extraction is decided and before it is returned, so an
    /// implementation sees every extraction exactly once and in order.
    virtual void on_extraction(std::uint64_t entries_skipped,
                               std::uint32_t verification_comparisons,
                               const CorrectionOutcome& outcome) noexcept = 0;
};

}  // namespace lpq

#endif  // LPQ_LPQ_EXTRACTION_OBSERVER_HPP
