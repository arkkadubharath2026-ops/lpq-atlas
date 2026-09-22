// =============================================================================
// Numerical comparison policy (ESD.md section 25).
//
// Path costs are compared in three different situations and they need three
// different rules. Conflating them is the classic source of "the test passes on
// -O2 and fails on -O3" in shortest-path code, so the rules are named here and
// used everywhere instead of ad-hoc `a == b` and `a < b + 1e-9`.
//
//   1. Integer weights        -- exact. No tolerance, ever.
//   2. Floating-point weights -- relative tolerance for *validation*, because
//                                two correct implementations may sum the same
//                                path in a different order.
//   3. Queue ordering         -- exact `<` on the key, always, even for double.
//                                Ordering must be a strict weak ordering; an
//                                epsilon-based comparator is not transitive and
//                                would corrupt heap invariants.
// =============================================================================
#ifndef LPQ_COMMON_NUMERIC_HPP
#define LPQ_COMMON_NUMERIC_HPP

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>

#include "lpq/common/types.hpp"

namespace lpq {

/// Default relative tolerance for comparing two independently computed
/// floating-point path costs.
///
/// Chosen as 1e-9: roughly four orders of magnitude above the ~2.2e-16 unit
/// roundoff of double, which absorbs the error of summing a few million edges
/// in a different association order, while remaining far tighter than any
/// approximation budget epsilon the experiments use (the smallest configured
/// epsilon is 1e-3). A discrepancy this policy accepts can therefore never be
/// mistaken for an approximation-quality signal.
inline constexpr double kDefaultRelativeTolerance = 1e-9;

/// Absolute floor, so that costs near zero do not require an impossible
/// relative match.
inline constexpr double kDefaultAbsoluteTolerance = 1e-12;

/// Relative-with-absolute-floor equality for floating-point costs.
[[nodiscard]] inline bool approx_equal(double a, double b,
                                       double rel_tol = kDefaultRelativeTolerance,
                                       double abs_tol = kDefaultAbsoluteTolerance) noexcept {
    if (a == b) {
        return true;  // Covers exact equality and the infinity == infinity case.
    }
    if (std::isnan(a) || std::isnan(b)) {
        return false;  // NaN never validates; it signals a bug upstream.
    }
    if (std::isinf(a) || std::isinf(b)) {
        return false;  // One is infinite and the other is not: genuinely different.
    }
    const double diff = std::fabs(a - b);
    return diff <= abs_tol || diff <= rel_tol * std::max(std::fabs(a), std::fabs(b));
}

/// True when `a <= b` within tolerance.
[[nodiscard]] inline bool approx_less_equal(double a, double b,
                                            double rel_tol = kDefaultRelativeTolerance,
                                            double abs_tol = kDefaultAbsoluteTolerance) noexcept {
    return a <= b || approx_equal(a, b, rel_tol, abs_tol);
}

// -----------------------------------------------------------------------------
// Cost comparison, dispatched on weight type.
//
// `costs_equal` is what every correctness check must use: exact for integers,
// tolerant for doubles. It is intentionally NOT usable as a queue comparator.
// -----------------------------------------------------------------------------
template <WeightType W>
[[nodiscard]] inline bool costs_equal(DistanceType<W> a, DistanceType<W> b,
                                      double rel_tol = kDefaultRelativeTolerance) noexcept {
    if constexpr (std::same_as<W, double>) {
        return approx_equal(a, b, rel_tol);
    } else {
        (void)rel_tol;
        return a == b;
    }
}

/// True when `a` is within a `(1 + epsilon)` factor of `b`, i.e. `a <= (1+eps)*b`.
///
/// This is the bounded-approximate contract of PRD.md section 4.2. The check is
/// performed in double for both weight families so that the same arithmetic
/// defines the guarantee regardless of the graph's weight type.
template <WeightType W>
[[nodiscard]] inline bool within_epsilon(DistanceType<W> returned_cost,
                                         DistanceType<W> optimal_cost,
                                         double epsilon) noexcept {
    const double r = static_cast<double>(returned_cost);
    const double o = static_cast<double>(optimal_cost);
    if (o == 0.0) {
        return r == 0.0;  // A zero-cost optimum admits no multiplicative slack.
    }
    return approx_less_equal(r, (1.0 + epsilon) * o);
}

/// Approximation ratio `returned / optimal`, as defined in PRD.md section 16.
///
/// Returns 1.0 for the degenerate `optimal == 0` case when the returned cost is
/// also zero, and infinity when it is not, so that a genuine failure cannot be
/// hidden behind a division by zero.
template <WeightType W>
[[nodiscard]] inline double approximation_ratio(DistanceType<W> returned_cost,
                                                DistanceType<W> optimal_cost) noexcept {
    const double r = static_cast<double>(returned_cost);
    const double o = static_cast<double>(optimal_cost);
    if (o == 0.0) {
        return r == 0.0 ? 1.0 : std::numeric_limits<double>::infinity();
    }
    return r / o;
}

// -----------------------------------------------------------------------------
// Saturating addition for distance accumulation.
//
// Relaxation computes `dist[u] + w`. When `dist[u]` is the unreachable sentinel
// this must not wrap around to a small value and create a phantom shorter path.
// Callers in the hot loop already guard on reachability, so this exists for the
// paths where the guard would cost more than the saturation (and for tests).
// -----------------------------------------------------------------------------
template <WeightType W>
[[nodiscard]] constexpr DistanceType<W> saturating_add(DistanceType<W> d, W w) noexcept {
    if constexpr (std::same_as<W, double>) {
        return d + w;  // IEEE infinity already saturates correctly.
    } else {
        using D = DistanceType<W>;
        const D limit = std::numeric_limits<D>::max();
        const D wd = static_cast<D>(w);
        return (d > limit - wd) ? limit : static_cast<D>(d + wd);
    }
}

/// Saturating addition of two distances.
///
/// A* forms its key as g + h, where both operands are distances rather than a
/// distance plus an edge weight. Without this overload the call would select
/// the one above and narrow the heuristic estimate from the 64-bit distance
/// type to the 32-bit weight type -- silently wrapping any estimate above
/// 2^32, which a Haversine heuristic in metres reaches easily on a
/// continental-scale road network.
template <WeightType W>
[[nodiscard]] constexpr DistanceType<W> saturating_add_distances(
    DistanceType<W> a, DistanceType<W> b) noexcept {
    if constexpr (std::same_as<W, double>) {
        return a + b;
    } else {
        using D = DistanceType<W>;
        const D limit = std::numeric_limits<D>::max();
        return (a > limit - b) ? limit : static_cast<D>(a + b);
    }
}

}  // namespace lpq

#endif  // LPQ_COMMON_NUMERIC_HPP
