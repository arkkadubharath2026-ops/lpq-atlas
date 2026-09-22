// =============================================================================
// Correctness policy and guarantee classification (PRD.md section 4, ESD.md
// section 17).
//
// ESD.md section 17 states a requirement that most of this file exists to
// satisfy:
//
//     The system must distinguish:
//       - formal guarantee,
//       - empirical guarantee,
//       - heuristic behavior.
//     Do not call a heuristic "guaranteed".
//
// PRD.md section 4.2 says the same thing from the other side: "The
// implementation must never silently label an empirical approximation as a
// formal guarantee. If a formal guarantee cannot be established for a specific
// mode, that mode must be documented as empirically approximate."
//
// So the guarantee level is a value the system carries and reports, not a
// claim made in prose somewhere. A mode that cannot prove its bound says so, in
// the results file, every time it runs.
//
// -----------------------------------------------------------------------------
// What is actually proven here, and what is not
// -----------------------------------------------------------------------------
//
// Two different statements are easy to conflate, and this project keeps them
// apart deliberately.
//
//   PER-EXTRACTION BOUND (formal).
//     Every extraction in bounded-approximate mode pops a node whose key is at
//     most (1 + epsilon) times the smallest key the queue could possibly hold
//     at that moment. This IS proven, by construction, in
//     VerificationEngine::verify -- the comparison is against a valid lower
//     bound on every key not examined, so no unexamined element can be smaller
//     than the bound the test used. See verification_engine.hpp.
//
//   END-TO-END PATH COST BOUND (not proven here).
//     That returned_cost <= (1 + epsilon) * optimal_cost follows from the
//     per-extraction bound is NOT established in this codebase. The usual
//     Dijkstra argument does not transfer directly: it relies on the extracted
//     key being exactly the node's final distance, and a relaxed extraction
//     breaks that, so the induction over a shortest path has to be redone under
//     the weaker hypothesis. Until that is done, bounded-approximate mode
//     reports kEmpirical.
//
// Reporting kEmpirical is not a placeholder to be quietly upgraded. Approximate
// runs compute the exact optimum with the reference implementation and report
// the MEASURED ratio (PRD.md section 16), which is a real result and does not
// depend on the proof existing. If the proof is later established, the
// classification changes in one place and the measurements stand unchanged.
// =============================================================================
#ifndef LPQ_VERIFICATION_CORRECTNESS_POLICY_HPP
#define LPQ_VERIFICATION_CORRECTNESS_POLICY_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "lpq/common/error.hpp"

namespace lpq {

/// Which correctness contract a run is operating under (PRD.md section 4).
enum class CorrectnessMode : std::uint8_t {
    /// The returned path must match a trusted reference exactly.
    kExact = 0,
    /// The user supplies an error budget epsilon (PRD.md section 4.2).
    kBoundedApproximate,
};

[[nodiscard]] inline constexpr std::string_view correctness_mode_name(CorrectnessMode m) noexcept {
    switch (m) {
        case CorrectnessMode::kExact:              return "exact";
        case CorrectnessMode::kBoundedApproximate: return "bounded_approximate";
    }
    return "unknown";
}

/// How strong the claim behind a mode's bound actually is (ESD.md section 17).
///
/// Serialised into every result record, so a number can never be read without
/// the status of the claim attached to it.
enum class GuaranteeKind : std::uint8_t {
    /// Proven by construction in this codebase.
    kFormal = 0,
    /// The mechanism is sound per step, but the end-to-end bound is measured
    /// rather than proven. Measurements are real; the proof is absent.
    kEmpirical,
    /// No bound is claimed at all.
    kHeuristic,
};

[[nodiscard]] inline constexpr std::string_view guarantee_kind_name(GuaranteeKind g) noexcept {
    switch (g) {
        case GuaranteeKind::kFormal:    return "formal";
        case GuaranteeKind::kEmpirical: return "empirical";
        case GuaranteeKind::kHeuristic: return "heuristic";
    }
    return "unknown";
}

/// A one-line statement of exactly what a mode does and does not promise.
///
/// Carried into the results file alongside the numbers. A reader should never
/// have to consult the source to learn whether a bound was proven.
[[nodiscard]] inline std::string guarantee_statement(CorrectnessMode mode) {
    switch (mode) {
        case CorrectnessMode::kExact:
            return "formal: every extraction is verified against a lower bound on all "
                   "unexamined keys, so the result is identical to exact Dijkstra/A*";
        case CorrectnessMode::kBoundedApproximate:
            return "empirical: each extraction is provably within (1+epsilon) of the "
                   "smallest possible key, but the end-to-end path-cost bound is not "
                   "proven in this codebase and is reported as a measured ratio";
    }
    return "unknown";
}

/// The correctness contract for a run (ESD.md section 17).
struct CorrectnessPolicy {
    CorrectnessMode mode = CorrectnessMode::kExact;

    /// Error budget. Meaningful only in bounded-approximate mode.
    ///
    /// Exactly zero in exact mode, and that is not merely a default: a
    /// non-zero epsilon under kExact would be a contradiction the run should
    /// not be allowed to start with, so validate() rejects it rather than
    /// silently ignoring the field.
    double epsilon = 0.0;

    /// Total verification work one extraction may perform before the caller
    /// must fall back to the authoritative queue (ESD.md section 17's
    /// "verification_budget").
    ///
    /// Bounded for the same reason the expansion count is: the failure this
    /// guards against is not a wrong answer but an unbounded amount of work to
    /// reach a right one, which presents as an unexplained slowdown rather
    /// than as an error.
    std::uint32_t verification_budget = 64;

    [[nodiscard]] GuaranteeKind guarantee() const noexcept {
        // See the header. Exact mode's bound is proven by construction;
        // approximate mode's end-to-end bound is not, so it reports empirical.
        return mode == CorrectnessMode::kExact ? GuaranteeKind::kFormal
                                               : GuaranteeKind::kEmpirical;
    }

    /// Multiplier applied to the lower bound when testing a candidate.
    /// Exactly 1 in exact mode, so the exact path costs no extra arithmetic.
    [[nodiscard]] double slack_factor() const noexcept { return 1.0 + epsilon; }

    void validate() const {
        if (!std::isfinite(epsilon) || epsilon < 0.0) {
            throw InvalidConfigError("epsilon must be finite and non-negative");
        }
        if (mode == CorrectnessMode::kExact && epsilon != 0.0) {
            throw InvalidConfigError(
                "exact mode cannot carry a non-zero epsilon; a run configured this way "
                "would report exact results while applying an approximation slack");
        }
        if (mode == CorrectnessMode::kBoundedApproximate && epsilon == 0.0) {
            throw InvalidConfigError(
                "bounded-approximate mode with epsilon = 0 is exact mode wearing the "
                "wrong label, and would be reported with a weaker guarantee than it "
                "actually provides; use CorrectnessMode::kExact");
        }
        if (verification_budget == 0) {
            throw InvalidConfigError("verification_budget must be at least 1");
        }
    }

    [[nodiscard]] static CorrectnessPolicy exact() noexcept { return {}; }

    [[nodiscard]] static CorrectnessPolicy approximate(double epsilon_value) {
        CorrectnessPolicy p;
        p.mode    = CorrectnessMode::kBoundedApproximate;
        p.epsilon = epsilon_value;
        p.validate();
        return p;
    }
};

}  // namespace lpq

#endif  // LPQ_VERIFICATION_CORRECTNESS_POLICY_HPP
