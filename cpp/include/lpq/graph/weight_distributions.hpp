// =============================================================================
// Synthetic edge weights (PRD.md section 17, ESD.md section 28).
//
// PRD.md section 17 requires the experiments to cover five weight
// distributions -- uniform, log-normal, exponential, narrow-range and
// heavy-tailed -- because the priority queues under test are not equally
// sensitive to them. A radix heap's bucket count is driven by the ratio of the
// largest to the smallest weight, so a narrow-range graph and a heavy-tailed
// graph of identical shape are different experiments for it. Measuring only one
// distribution would silently pick a winner.
//
// This header is shared by two callers that must agree:
//
//   * the dataset converter, which synthesises weights for SNAP graphs (they
//     are distributed unweighted, so a travel cost has to come from somewhere),
//   * the synthetic graph generator (ESD.md section 28).
//
// -----------------------------------------------------------------------------
// Why weights are hashed rather than drawn from a stream
// -----------------------------------------------------------------------------
//
// The obvious implementation draws from a seeded Rng while walking the arcs.
// That is wrong here for two independent reasons.
//
// First, SYMMETRY. An undirected graph is stored as paired directed arcs, and
// (u,v) and (v,u) are visited at different points in the walk, so a stream
// would give them different weights. The graph would stop being undirected --
// quietly, and in a way that changes shortest-path results rather than crashing.
//
// Second, ORDER INDEPENDENCE. A stream ties each weight to its position in the
// traversal, so reordering the input edge list -- which a reconversion, a
// different reader, or a parallel-arc collapse can do -- changes every weight
// downstream of the first difference. A query manifest generated against one
// conversion would then refer to a different graph.
//
// Deriving each weight from a hash of the UNORDERED endpoint pair and the seed
// fixes both: w(u,v) == w(v,u) by construction, and the weight of an edge
// depends on nothing but the edge itself and the seed.
//
// The cost is that parallel arcs between the same pair receive identical
// weights. That is stated rather than worked around: distinguishing them would
// require an occurrence index, which is exactly the order-dependent quantity
// this design is avoiding.
// =============================================================================
#ifndef LPQ_GRAPH_WEIGHT_DISTRIBUTIONS_HPP
#define LPQ_GRAPH_WEIGHT_DISTRIBUTIONS_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_builder.hpp"

namespace lpq {

/// The weight families PRD.md section 17 requires.
enum class WeightDistribution : std::uint8_t {
    /// Every edge weighs the same. The control: any difference between queues
    /// on a constant-weight graph is structural, not distributional.
    kConstant,
    kUniform,
    kLogNormal,
    kExponential,
    /// Uniform over a deliberately tight band around the midpoint. Separated
    /// from kUniform because "narrow range" is a property of the RATIO of
    /// maximum to minimum, which is what a radix heap is sensitive to, and
    /// expressing it by shrinking a uniform range would conflate it with scale.
    kNarrowRange,
    /// Pareto. Unbounded in principle, clamped to the configured maximum in
    /// practice; see sample_weight().
    kHeavyTailed,
};

[[nodiscard]] inline std::string_view weight_distribution_name(WeightDistribution d) noexcept {
    switch (d) {
        case WeightDistribution::kConstant:    return "constant";
        case WeightDistribution::kUniform:     return "uniform";
        case WeightDistribution::kLogNormal:   return "lognormal";
        case WeightDistribution::kExponential: return "exponential";
        case WeightDistribution::kNarrowRange: return "narrow_range";
        case WeightDistribution::kHeavyTailed: return "heavy_tailed";
    }
    return "unknown";
}

/// Parses a distribution name. Returns false for an unknown name rather than
/// falling back to a default, so a typo in a dataset config cannot silently
/// change which experiment was run.
[[nodiscard]] inline bool parse_weight_distribution(std::string_view name,
                                                    WeightDistribution& out) noexcept {
    if (name == "constant")     { out = WeightDistribution::kConstant;    return true; }
    if (name == "uniform")      { out = WeightDistribution::kUniform;     return true; }
    if (name == "lognormal")    { out = WeightDistribution::kLogNormal;   return true; }
    if (name == "exponential")  { out = WeightDistribution::kExponential; return true; }
    if (name == "narrow_range") { out = WeightDistribution::kNarrowRange; return true; }
    if (name == "heavy_tailed") { out = WeightDistribution::kHeavyTailed; return true; }
    return false;
}

/// Parameters of a synthetic weight assignment.
///
/// `minimum` and `maximum` are inclusive bounds on the produced weight. Every
/// family is mapped into them, so no configuration can emit a zero weight where
/// a positive one was intended, and none can overflow the weight type.
struct WeightDistributionConfig {
    WeightDistribution kind    = WeightDistribution::kConstant;
    double             minimum = 1.0;
    double             maximum = 1.0;

    /// Log-normal shape. The underlying normal has standard deviation `sigma`;
    /// its mean is chosen so the median lands at the geometric centre of
    /// [minimum, maximum].
    double sigma = 1.0;

    /// Pareto tail index for kHeavyTailed. Below 2 the variance is infinite and
    /// below 1 the mean is too, which is the point of the family.
    double alpha = 1.5;

    /// Half-width of kNarrowRange, as a fraction of the midpoint. 0.05 means
    /// weights land within +/-5% of the midpoint.
    double narrow_fraction = 0.05;

    SeedType seed = 0;

    /// Rejects a configuration that cannot produce a valid weight, at the point
    /// where the dataset config is read rather than deep inside a conversion.
    void validate() const {
        if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
            throw InvalidConfigError("weight bounds must be finite");
        }
        if (minimum < 0.0) {
            throw InvalidConfigError(
                "weight minimum must be non-negative: Dijkstra's correctness argument "
                "depends on it");
        }
        if (maximum < minimum) {
            throw InvalidConfigError("weight maximum is below weight minimum");
        }
        if (kind == WeightDistribution::kLogNormal && !(sigma > 0.0 && std::isfinite(sigma))) {
            throw InvalidConfigError("lognormal sigma must be positive and finite");
        }
        if (kind == WeightDistribution::kHeavyTailed && !(alpha > 0.0 && std::isfinite(alpha))) {
            throw InvalidConfigError("heavy-tailed alpha must be positive and finite");
        }
        if (kind == WeightDistribution::kNarrowRange &&
            !(narrow_fraction >= 0.0 && narrow_fraction <= 1.0)) {
            throw InvalidConfigError("narrow_fraction must lie in [0, 1]");
        }
    }
};

namespace detail {

/// SplitMix64's finaliser. An avalanche mix, so two endpoint pairs that differ
/// in one bit produce unrelated weights -- adjacent node ids are extremely
/// common in these datasets and must not yield correlated weights.
[[nodiscard]] inline constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// A hash of the UNORDERED pair {a, b} together with the seed.
///
/// The endpoints are sorted before mixing, which is what makes w(u,v) equal
/// w(v,u) and therefore what keeps an undirected graph undirected.
[[nodiscard]] inline std::uint64_t edge_hash(NodeId a, NodeId b, SeedType seed) noexcept {
    const std::uint64_t lo = std::min(a, b);
    const std::uint64_t hi = std::max(a, b);
    return mix64(mix64(lo ^ mix64(seed)) ^ (hi + 0x9E3779B97F4A7C15ULL));
}

/// Maps a hash onto [0, 1). The top 53 bits are used because that is exactly
/// the mantissa width of a double, so every representable value in the interval
/// is reachable and none is produced twice as often as its neighbours.
[[nodiscard]] inline double unit_from_hash(std::uint64_t h) noexcept {
    return static_cast<double>(h >> 11) * 0x1.0p-53;
}

}  // namespace detail

/// Draws one weight for the edge {a, b}.
///
/// Pure: the same endpoints, config and seed always give the same weight, on
/// any platform with IEEE-754 doubles.
[[nodiscard]] inline double sample_weight(const WeightDistributionConfig& cfg, NodeId a,
                                          NodeId b) {
    const double lo = cfg.minimum;
    const double hi = cfg.maximum;

    if (cfg.kind == WeightDistribution::kConstant || hi <= lo) {
        return lo;
    }

    const std::uint64_t h1 = detail::edge_hash(a, b, cfg.seed);
    const double        u1 = detail::unit_from_hash(h1);

    double value = lo;
    switch (cfg.kind) {
        case WeightDistribution::kConstant:
            value = lo;
            break;

        case WeightDistribution::kUniform:
            value = lo + u1 * (hi - lo);
            break;

        case WeightDistribution::kNarrowRange: {
            const double mid   = 0.5 * (lo + hi);
            const double delta = mid * cfg.narrow_fraction;
            value              = (mid - delta) + u1 * (2.0 * delta);
            break;
        }

        case WeightDistribution::kExponential: {
            // Inverse transform. 1 - u1 rather than u1 so the argument of the
            // logarithm is never zero: u1 can be exactly 0, but never exactly 1.
            //
            // The rate is set so the mean sits one quarter of the way into the
            // range, which puts the bulk of the mass low and a long thin tail
            // towards the maximum -- the shape that actually stresses a bucket
            // queue, rather than an exponential that is effectively uniform
            // after clamping.
            const double mean  = 0.25 * (hi - lo);
            const double draw  = -std::log(1.0 - u1) * mean;
            value              = lo + draw;
            break;
        }

        case WeightDistribution::kLogNormal: {
            // Box-Muller from two independent uniforms. The second comes from
            // re-mixing the first hash rather than from a stream, so the draw
            // stays a pure function of the edge.
            const double u2 = detail::unit_from_hash(detail::mix64(h1));
            const double r  = std::sqrt(-2.0 * std::log(1.0 - u1));
            const double z  = r * std::cos(2.0 * 3.14159265358979323846 * u2);

            // Median at the geometric centre of the range, so the distribution
            // is symmetric in log space about the middle of the interval the
            // caller asked for.
            const double log_lo = std::log(std::max(lo, 1e-12));
            const double log_hi = std::log(hi);
            const double mu     = 0.5 * (log_lo + log_hi);
            value               = std::exp(mu + cfg.sigma * z);
            break;
        }

        case WeightDistribution::kHeavyTailed: {
            // Pareto by inverse transform, anchored at the minimum. Unbounded
            // in principle; the clamp below truncates it, which is a deliberate
            // and documented departure from the pure family -- an unclamped
            // draw can exceed the weight type, and an overflowing weight is a
            // silently wrong graph rather than a heavy tail.
            value = lo * std::pow(1.0 - u1, -1.0 / cfg.alpha);
            break;
        }
    }

    if (!std::isfinite(value)) {
        return hi;
    }
    return std::clamp(value, lo, hi);
}

/// Rounds a sampled weight into the graph's weight type.
///
/// Integer weight types round to nearest; a weight that rounds to zero is
/// raised to one when the configured minimum was positive, because an edge that
/// was meant to cost something must not become free.
template <WeightType W>
[[nodiscard]] W quantize_weight(double value, const WeightDistributionConfig& cfg) noexcept {
    if constexpr (std::is_floating_point_v<W>) {
        return static_cast<W>(value);
    } else {
        const double limit   = static_cast<double>(std::numeric_limits<W>::max());
        const double rounded = std::floor(std::clamp(value, 0.0, limit) + 0.5);
        auto         w       = static_cast<W>(rounded);
        if (w == W{0} && cfg.minimum > 0.0) {
            w = W{1};
        }
        return w;
    }
}

/// Returns a copy of `g` with every weight replaced by a synthetic draw.
///
/// Structure is untouched: the same arcs, in the same adjacency lists. The
/// result is rebuilt through GraphBuilder rather than by overwriting the weight
/// array in place, both because the canonical intra-adjacency order is
/// (target, weight) -- so changing weights can change the order -- and because
/// rebuilding keeps the builder's validation on the only path that produces a
/// graph.
template <WeightType W>
[[nodiscard]] CsrGraph<W> synthesize_weights(const CsrGraph<W>& g,
                                             const WeightDistributionConfig& cfg,
                                             const std::vector<Coordinate>& coordinates = {},
                                             CoordinateSystem coord_system =
                                                 CoordinateSystem::kNone) {
    cfg.validate();

    std::vector<InputEdge<W>> edges;
    edges.reserve(g.num_arcs());

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const Arc<W>& a : g.out_arcs(u)) {
            const double sampled = sample_weight(cfg, u, a.target);
            edges.push_back(InputEdge<W>{u, a.target, quantize_weight<W>(sampled, cfg)});
        }
    }

    GraphBuildOptions opts;
    // The arcs come from an existing CSR graph, so both directions of every
    // undirected edge are already present. The label is carried over so that a
    // reweighted graph still reports the edge count of the graph it came from.
    opts.directedness        = g.directedness();
    opts.arcs_already_paired = true;
    opts.parallel_edges      = ParallelEdgePolicy::kKeep;
    opts.self_loops          = SelfLoopPolicy::kKeep;
    opts.coord_system = coordinates.empty() ? CoordinateSystem::kNone : coord_system;

    std::vector<Coordinate> coords = coordinates;
    return GraphBuilder<W>::build(g.num_nodes(), std::move(edges), opts, coords);
}

}  // namespace lpq

#endif  // LPQ_GRAPH_WEIGHT_DISTRIBUTIONS_HPP
