// =============================================================================
// Approximation measurement (PRD.md section 16, PRD.md section 4.2, ESD.md
// section 17).
//
// PRD.md section 16 is prescriptive about approximate mode:
//
//     For approximate mode:
//       - Compute exact optimum with the reference implementation.
//       - Report approximation ratio: returned_cost / optimal_cost
//
// This header computes that ratio against the trusted reference and summarises
// it. It is the only place in the project that is allowed to say how good an
// approximation was, and it says so from measurement rather than from the
// configured epsilon.
//
// -----------------------------------------------------------------------------
// The distinction this file exists to protect
// -----------------------------------------------------------------------------
//
// A run configured with epsilon = 0.1 has NOT been shown to return paths within
// 10% of optimal. It has been shown to accept, at each extraction, a key within
// 10% of the smallest key the queue could hold -- which is a different claim
// (see correctness_policy.hpp). Whether the first follows from the second is
// not proven in this codebase.
//
// So the configured epsilon is a BUDGET and the measured ratio is a RESULT, and
// they are kept in separate fields with separate names. `exceeded_budget` is
// reported rather than asserted: a run whose measured ratio goes above its
// budget is a finding about the method, not a bug in the harness, and silently
// failing on it would hide exactly the evidence that decides whether the
// end-to-end bound holds.
//
// -----------------------------------------------------------------------------
// Ratios for degenerate pairs
// -----------------------------------------------------------------------------
//
// Two cases have no natural ratio and both are handled explicitly rather than
// papered over:
//
//   * optimal cost zero (the source itself, or a zero-weight path). A ratio
//     would divide by zero. Recorded as 1.0 when the returned cost is also
//     zero, and as a hard mismatch otherwise, because returning a positive
//     cost where zero was optimal is a correctness failure and not a small
//     approximation.
//
//   * unreachable targets. If both agree the target is unreachable there is
//     nothing to approximate and the pair is excluded from the ratio
//     statistics -- including it as 1.0 would dilute the summary with
//     non-results and make a bad approximation look better the more
//     disconnected the graph was. A disagreement about reachability is counted
//     separately as a correctness failure: no epsilon licenses inventing or
//     losing a path.
// =============================================================================
#ifndef LPQ_VERIFICATION_APPROXIMATION_REPORT_HPP
#define LPQ_VERIFICATION_APPROXIMATION_REPORT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/metrics/statistics.hpp"

namespace lpq {

/// One measured (returned, optimal) pair.
template <WeightType W>
struct ApproximationSample {
    NodeId           target   = 0;
    DistanceType<W>  returned = 0;
    DistanceType<W>  optimal  = 0;
    double           ratio    = 1.0;
};

/// Summary of an approximate run against the trusted reference.
struct ApproximationReport {
    /// Targets that contributed a ratio.
    std::uint64_t compared = 0;

    /// Targets both implementations agreed were unreachable. Excluded from the
    /// ratio statistics; see the header.
    std::uint64_t both_unreachable = 0;

    /// Targets where one implementation found a path and the other did not.
    /// A correctness failure at any epsilon.
    std::uint64_t reachability_mismatches = 0;

    /// Targets where the optimum was zero but the returned cost was not.
    /// Also a correctness failure, not an approximation.
    std::uint64_t zero_cost_mismatches = 0;

    /// Targets where the returned cost was BELOW the reference optimum.
    /// Impossible for a correct implementation, and therefore evidence of a bug
    /// in the search or in the reference rather than of a good approximation.
    std::uint64_t below_optimal = 0;

    double max_ratio    = 1.0;
    double mean_ratio   = 1.0;
    double median_ratio = 1.0;

    /// Median absolute deviation of the ratio. Reported alongside the median
    /// because a single pathological target can move a mean substantially while
    /// leaving the median intact, and the distribution is the result here.
    double mad_ratio = 0.0;

    double p95_ratio = 1.0;
    double p99_ratio = 1.0;

    /// The epsilon the run was CONFIGURED with. A budget, not a result.
    double configured_epsilon = 0.0;

    /// Whether any measured ratio exceeded 1 + configured_epsilon.
    ///
    /// Reported, never asserted. A run that exceeds its budget is a finding
    /// about the method -- the end-to-end bound is not proven in this codebase
    /// -- and failing on it would suppress the evidence that decides the
    /// question.
    bool exceeded_budget = false;

    /// True when nothing was found that would be wrong at ANY epsilon.
    [[nodiscard]] bool correctness_intact() const noexcept {
        return reachability_mismatches == 0 && zero_cost_mismatches == 0 && below_optimal == 0;
    }
};

/// Compares a run's distances against the reference's and summarises the ratios.
///
/// Both vectors must be indexed by node id and have the same length.
template <WeightType W>
[[nodiscard]] ApproximationReport measure_approximation(
    const std::vector<DistanceType<W>>& returned, const std::vector<DistanceType<W>>& optimal,
    double configured_epsilon, std::vector<ApproximationSample<W>>* samples = nullptr) {
    ApproximationReport report;
    report.configured_epsilon = configured_epsilon;

    std::vector<double> ratios;
    ratios.reserve(returned.size());

    const std::size_t n = std::min(returned.size(), optimal.size());
    for (std::size_t u = 0; u < n; ++u) {
        const DistanceType<W> got  = returned[u];
        const DistanceType<W> want = optimal[u];

        const bool got_unreachable  = (got == kUnreachable<W>);
        const bool want_unreachable = (want == kUnreachable<W>);

        if (got_unreachable && want_unreachable) {
            ++report.both_unreachable;
            continue;
        }
        if (got_unreachable != want_unreachable) {
            // No epsilon licenses inventing or losing a path.
            ++report.reachability_mismatches;
            continue;
        }

        if (want == 0) {
            if (got != 0) {
                ++report.zero_cost_mismatches;
            } else {
                // Both zero: nothing to approximate, and a ratio would divide
                // by zero. Counted as compared with ratio 1 so the source node
                // is not silently dropped from the accounting.
                ++report.compared;
                ratios.push_back(1.0);
            }
            continue;
        }

        if (got < want) {
            // Cheaper than optimal is not a good approximation; it means one of
            // the two implementations is wrong.
            ++report.below_optimal;
            continue;
        }

        const double ratio = static_cast<double>(got) / static_cast<double>(want);
        ++report.compared;
        ratios.push_back(ratio);

        if (samples != nullptr) {
            samples->push_back(
                ApproximationSample<W>{static_cast<NodeId>(u), got, want, ratio});
        }
    }

    if (ratios.empty()) {
        return report;
    }

    const SampleSummary summary = summarise(ratios);
    report.max_ratio            = summary.max;
    report.mean_ratio           = summary.mean;
    report.median_ratio         = summary.median;
    report.mad_ratio            = summary.mad;
    report.p95_ratio            = summary.p95;
    report.p99_ratio            = summary.p99;
    report.exceeded_budget      = summary.max > (1.0 + configured_epsilon);

    return report;
}

}  // namespace lpq

#endif  // LPQ_VERIFICATION_APPROXIMATION_REPORT_HPP
