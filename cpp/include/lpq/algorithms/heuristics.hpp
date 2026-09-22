// =============================================================================
// A* heuristics (ESD.md section 9).
//
// ESD.md section 9 names four: zero (equivalent to Dijkstra), Euclidean for
// spatial graphs, Haversine for geographic OSM data, and optionally Manhattan
// for grids.
//
// -----------------------------------------------------------------------------
// Two properties, and why the distinction is load-bearing here
// -----------------------------------------------------------------------------
//
//   ADMISSIBLE   h(v) <= true cost from v to the target, for every v.
//                Guarantees A* returns an optimal path.
//
//   CONSISTENT   h(u) <= w(u,v) + h(v) for every arc, and h(target) == 0.
//                Strictly stronger. Guarantees additionally that the key
//                sequence f = g + h is non-decreasing along the search, so a
//                node's first extraction is final and it never needs
//                re-expansion.
//
// Most treatments mention consistency in passing. It matters concretely in this
// project for two reasons:
//
//   1. The radix heap requires monotone keys. With a merely admissible
//      heuristic, f can decrease between extractions, which would place an
//      entry in an already-drained bucket where it is silently lost -- a wrong
//      answer, not a crash. Each heuristic therefore declares whether it is
//      consistent, and the runner refuses to pair a non-consistent heuristic
//      with the radix heap rather than producing invalid results.
//
//   2. With a consistent heuristic, A* can settle each node once, exactly like
//      Dijkstra. Without consistency it must allow re-expansion, which is a
//      different algorithm with different operation counts. Conflating the two
//      would corrupt the comparison.
//
// Every heuristic here is consistent, so both are currently available. The
// machinery exists because the learned-heuristic experiment (ESD.md section 9,
// out of the primary LPQ path) will produce heuristics that are neither.
//
// -----------------------------------------------------------------------------
// Integer weights
// -----------------------------------------------------------------------------
//
// Geometric distances are real-valued while integer-weight graphs need an
// integral key. The value is FLOORED, never rounded: floor(h) <= h <= true
// cost, so flooring preserves admissibility, whereas rounding to nearest can
// round up past the true cost and destroy optimality on a graph whose weights
// are small integers.
// =============================================================================
#ifndef LPQ_ALGORITHMS_HEURISTICS_HPP
#define LPQ_ALGORITHMS_HEURISTICS_HPP

#include <cmath>
#include <concepts>
#include <cstdint>
#include <string>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq {

/// What a heuristic must provide.
///
/// `prepare` is called once per query with the target, so a heuristic can cache
/// the target's coordinates rather than looking them up per node -- this is the
/// hottest non-queue call in A*.
template <typename H, typename W>
concept HeuristicLike = requires(H h, const H ch, const CsrGraph<W>& g, NodeId v, NodeId t) {
    { h.prepare(g, t) } -> std::same_as<void>;
    { ch.estimate(v) } -> std::convertible_to<DistanceType<W>>;
    { H::kIsConsistent } -> std::convertible_to<bool>;
    { ch.name() } -> std::convertible_to<std::string>;
};

namespace detail {

/// Converts a real-valued estimate into the graph's distance type.
///
/// Floors for integer types, preserving admissibility. Also clamps at zero: a
/// negative estimate is meaningless and would make the key smaller than g,
/// breaking the search's ordering argument.
template <WeightType W>
[[nodiscard]] inline DistanceType<W> to_distance(double estimate) noexcept {
    if (!(estimate > 0.0)) {
        return DistanceType<W>{};  // covers negatives and NaN
    }
    if constexpr (std::same_as<W, double>) {
        return estimate;
    } else {
        using D = DistanceType<W>;
        const double limit = static_cast<double>(std::numeric_limits<D>::max());
        if (estimate >= limit) {
            return std::numeric_limits<D>::max();
        }
        return static_cast<D>(std::floor(estimate));
    }
}

}  // namespace detail

// -----------------------------------------------------------------------------
// Zero heuristic
// -----------------------------------------------------------------------------

/// h(v) = 0 for all v. A* with this is exactly Dijkstra.
///
/// Kept as a first-class heuristic rather than a special case, because it is
/// the control in the A* experiments: any A* speedup must be measured against
/// A*-with-zero-heuristic, not only against Dijkstra, so that the comparison
/// isolates the heuristic's contribution from any difference in the A*
/// implementation itself.
template <WeightType W>
class ZeroHeuristic {
public:
    static constexpr bool kIsConsistent = true;

    void prepare(const CsrGraph<W>&, NodeId) noexcept {}

    [[nodiscard]] DistanceType<W> estimate(NodeId) const noexcept {
        return DistanceType<W>{};
    }

    [[nodiscard]] std::string name() const { return "zero"; }
};

// -----------------------------------------------------------------------------
// Euclidean
// -----------------------------------------------------------------------------

/// Straight-line distance in the plane, scaled into weight units.
///
/// `scale` converts coordinate units into weight units and MUST NOT exceed the
/// minimum weight-per-coordinate-unit ratio anywhere in the graph, or the
/// estimate overshoots the true cost and A* returns non-optimal paths. The
/// default of 1.0 is correct for a unit-weight grid whose coordinates are cell
/// indices; `euclidean_admissible_scale` below derives a safe value from the
/// graph.
template <WeightType W>
class EuclideanHeuristic {
public:
    static constexpr bool kIsConsistent = true;

    explicit EuclideanHeuristic(double scale = 1.0) : scale_(scale) {
        if (!(scale > 0.0)) {
            throw InvalidConfigError("euclidean heuristic scale must be positive");
        }
    }

    void prepare(const CsrGraph<W>& g, NodeId target) {
        if (!g.has_coordinates()) {
            throw UnsupportedConfigurationError(
                "euclidean heuristic requires node coordinates");
        }
        coords_ = g.coordinates().data();
        if (target < g.num_nodes()) {
            tx_ = coords_[target].x;
            ty_ = coords_[target].y;
        } else {
            tx_ = ty_ = 0.0;
        }
    }

    [[nodiscard]] DistanceType<W> estimate(NodeId v) const noexcept {
        const double dx = coords_[v].x - tx_;
        const double dy = coords_[v].y - ty_;
        return detail::to_distance<W>(std::sqrt(dx * dx + dy * dy) * scale_);
    }

    [[nodiscard]] std::string name() const { return "euclidean"; }

private:
    const Coordinate* coords_ = nullptr;
    double            tx_     = 0.0;
    double            ty_     = 0.0;
    double            scale_  = 1.0;
};

// -----------------------------------------------------------------------------
// Manhattan
// -----------------------------------------------------------------------------

/// |dx| + |dy|, scaled. Admissible and consistent on a 4-connected grid, where
/// no arc moves diagonally so no path can be shorter than the L1 distance.
///
/// NOT admissible on a graph with diagonal arcs, where the true cost can fall
/// below the L1 distance. The runner pairs it only with grid datasets.
template <WeightType W>
class ManhattanHeuristic {
public:
    static constexpr bool kIsConsistent = true;

    explicit ManhattanHeuristic(double scale = 1.0) : scale_(scale) {
        if (!(scale > 0.0)) {
            throw InvalidConfigError("manhattan heuristic scale must be positive");
        }
    }

    void prepare(const CsrGraph<W>& g, NodeId target) {
        if (!g.has_coordinates()) {
            throw UnsupportedConfigurationError(
                "manhattan heuristic requires node coordinates");
        }
        coords_ = g.coordinates().data();
        if (target < g.num_nodes()) {
            tx_ = coords_[target].x;
            ty_ = coords_[target].y;
        } else {
            tx_ = ty_ = 0.0;
        }
    }

    [[nodiscard]] DistanceType<W> estimate(NodeId v) const noexcept {
        const double d = std::fabs(coords_[v].x - tx_) + std::fabs(coords_[v].y - ty_);
        return detail::to_distance<W>(d * scale_);
    }

    [[nodiscard]] std::string name() const { return "manhattan"; }

private:
    const Coordinate* coords_ = nullptr;
    double            tx_     = 0.0;
    double            ty_     = 0.0;
    double            scale_  = 1.0;
};

// -----------------------------------------------------------------------------
// Haversine
// -----------------------------------------------------------------------------

/// Mean Earth radius in metres (IUGG). Used by the Haversine heuristic.
inline constexpr double kEarthRadiusMetres = 6371008.8;

/// Great-circle distance for geographic graphs, scaled into weight units.
///
/// Coordinates are degrees: x is longitude, y is latitude.
///
/// Haversine is used rather than the more accurate Vincenty because it is the
/// one that stays ADMISSIBLE. Haversine models a sphere, and the Earth's
/// ellipsoid is everywhere at least as long as the sphere of mean radius along
/// a geodesic, so the spherical distance is an underestimate. A more accurate
/// ellipsoidal formula can exceed the travel cost implied by the graph's
/// weights and break optimality -- accuracy and admissibility are different
/// goals, and only the latter is required for correctness.
///
/// `scale` converts metres into weight units, typically 1/max_speed for a
/// travel-time graph. Overestimating it destroys optimality, so the runner
/// derives it from the dataset rather than defaulting it.
template <WeightType W>
class HaversineHeuristic {
public:
    static constexpr bool kIsConsistent = true;

    explicit HaversineHeuristic(double scale = 1.0) : scale_(scale) {
        if (!(scale > 0.0)) {
            throw InvalidConfigError("haversine heuristic scale must be positive");
        }
    }

    void prepare(const CsrGraph<W>& g, NodeId target) {
        if (!g.has_coordinates()) {
            throw UnsupportedConfigurationError(
                "haversine heuristic requires node coordinates");
        }
        if (g.coordinate_system() != CoordinateSystem::kGeographic) {
            throw UnsupportedConfigurationError(
                "haversine heuristic requires geographic coordinates (degrees)");
        }
        coords_ = g.coordinates().data();
        if (target < g.num_nodes()) {
            t_lat_rad_ = coords_[target].y * kDegToRad;
            t_lon_rad_ = coords_[target].x * kDegToRad;
            cos_t_lat_ = std::cos(t_lat_rad_);
        }
    }

    [[nodiscard]] DistanceType<W> estimate(NodeId v) const noexcept {
        const double lat = coords_[v].y * kDegToRad;
        const double lon = coords_[v].x * kDegToRad;

        const double dlat = t_lat_rad_ - lat;
        const double dlon = t_lon_rad_ - lon;

        const double sin_dlat = std::sin(dlat * 0.5);
        const double sin_dlon = std::sin(dlon * 0.5);

        // The standard haversine form. Clamped below because rounding can push
        // `a` a hair above 1 for antipodal points, and sqrt of a negative
        // 1 - a would yield NaN -- which would make every comparison false and
        // silently corrupt the heap ordering.
        double a = sin_dlat * sin_dlat + std::cos(lat) * cos_t_lat_ * sin_dlon * sin_dlon;
        a        = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);

        const double c = 2.0 * std::asin(std::sqrt(a));
        return detail::to_distance<W>(c * kEarthRadiusMetres * scale_);
    }

    [[nodiscard]] std::string name() const { return "haversine"; }

private:
    static constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

    const Coordinate* coords_    = nullptr;
    double            t_lat_rad_ = 0.0;
    double            t_lon_rad_ = 0.0;
    double            cos_t_lat_ = 1.0;
    double            scale_     = 1.0;
};

// -----------------------------------------------------------------------------
// Deriving an admissible scale
// -----------------------------------------------------------------------------

/// Largest scale factor for which a Euclidean heuristic stays admissible.
///
/// Scans every arc and returns min(weight / euclidean_length). Any larger scale
/// would let the estimate exceed the cost of some arc, and a heuristic that
/// overestimates even one arc can make A* return a non-optimal path.
///
/// Arcs of zero geometric length are skipped: they impose no constraint, since
/// any scale times zero is zero. Returns 0 when no arc constrains the scale, in
/// which case the caller should use the zero heuristic -- a scale of zero makes
/// the heuristic useless but never wrong.
///
/// O(m), run once per dataset at load time, never in a timed region.
template <WeightType W>
[[nodiscard]] inline double euclidean_admissible_scale(const CsrGraph<W>& g) {
    if (!g.has_coordinates()) {
        return 0.0;
    }
    double best = std::numeric_limits<double>::infinity();
    bool   any  = false;

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        const auto& cu = g.coordinate(u);
        for (const Arc<W>& a : g.out_arcs(u)) {
            const auto&  cv = g.coordinate(a.target);
            const double dx = cv.x - cu.x;
            const double dy = cv.y - cu.y;
            const double len = std::sqrt(dx * dx + dy * dy);
            if (len <= 0.0) {
                continue;
            }
            const double ratio = static_cast<double>(a.weight) / len;
            if (ratio < best) {
                best = ratio;
            }
            any = true;
        }
    }
    return any && std::isfinite(best) ? best : 0.0;
}

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_HEURISTICS_HPP
