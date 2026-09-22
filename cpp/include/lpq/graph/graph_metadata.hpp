// =============================================================================
// Graph metadata (ESD.md section 5).
//
// Every dataset carries a metadata record describing its size, weight range and
// degree distribution. Three separate parts of the system consume it:
//
//   * The benchmark runner, which writes graph_size and edge_count into every
//     result record (ESD.md section 23).
//   * The radix heap, which needs the maximum weight to size its buckets and to
//     decide whether the monotonic-integer precondition holds (ESD.md section 7).
//   * The feature extractor, which normalises node-level features against
//     graph-level degree and weight statistics (PRD.md section 9).
//
// Computing it is a single linear pass, done once at load time and never in a
// timed region.
// =============================================================================
#ifndef LPQ_GRAPH_GRAPH_METADATA_HPP
#define LPQ_GRAPH_GRAPH_METADATA_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq {

/// Summary statistics describing a loaded graph.
struct GraphMetadata {
    std::string      dataset_id;
    std::size_t      node_count   = 0;
    std::size_t      edge_count   = 0;  ///< Semantic edges (halved when undirected).
    std::size_t      arc_count    = 0;  ///< Stored directed arcs.
    Directedness     directedness = Directedness::kDirected;
    WeightKind       weight_kind  = WeightKind::kUInt32;
    CoordinateSystem coord_system = CoordinateSystem::kNone;

    // --- Weight statistics (over stored arcs) ---------------------------------
    double min_weight  = 0.0;
    double max_weight  = 0.0;
    double mean_weight = 0.0;
    double stddev_weight = 0.0;
    /// True when every stored weight is zero. Such a graph is a legal input but
    /// makes every reachable node tie at distance zero, which is a deliberate
    /// stress case for tie-breaking policy.
    bool all_weights_zero = false;
    /// True when no stored weight is zero. The radix heap's monotonic bucket
    /// argument is simplest under strictly positive weights.
    bool all_weights_positive = false;

    // --- Degree statistics ----------------------------------------------------
    std::size_t min_out_degree = 0;
    std::size_t max_out_degree = 0;
    double      mean_out_degree = 0.0;
    double      stddev_out_degree = 0.0;
    std::size_t isolated_nodes = 0;  ///< Nodes with out-degree zero.

    // --- Derived --------------------------------------------------------------
    /// Arcs divided by nodes. Reported because runtime on sparse road networks
    /// and on dense random graphs is driven by different costs.
    [[nodiscard]] double density() const noexcept {
        return node_count == 0 ? 0.0
                               : static_cast<double>(arc_count) / static_cast<double>(node_count);
    }

    /// Upper bound on any simple path's cost, used to size radix-heap buckets.
    ///
    /// Deliberately the loose bound (n - 1) * max_weight rather than a tighter
    /// estimate: it is guaranteed correct for any graph, and the radix heap only
    /// needs it to pick a bucket count, which is logarithmic in the bound.
    [[nodiscard]] double max_path_cost_bound() const noexcept {
        return node_count <= 1 ? 0.0 : static_cast<double>(node_count - 1) * max_weight;
    }
};

/// Computes metadata in one pass over the CSR arrays.
template <WeightType W>
[[nodiscard]] GraphMetadata compute_metadata(const CsrGraph<W>& g) {
    GraphMetadata m;
    m.dataset_id   = g.dataset_id();
    m.node_count   = g.num_nodes();
    m.edge_count   = g.num_edges();
    m.arc_count    = g.num_arcs();
    m.directedness = g.directedness();
    m.weight_kind  = CsrGraph<W>::weight_kind();
    m.coord_system = g.coordinate_system();

    // --- Weights -------------------------------------------------------------
    if (m.arc_count > 0) {
        double sum    = 0.0;
        double sum_sq = 0.0;
        double lo     = static_cast<double>(g.arcs()[0].weight);
        double hi     = lo;
        bool   any_nonzero = false;
        bool   any_zero    = false;

        for (const Arc<W>& a : g.arcs()) {
            const double w = static_cast<double>(a.weight);
            lo  = std::min(lo, w);
            hi  = std::max(hi, w);
            sum += w;
            sum_sq += w * w;
            if (w == 0.0) {
                any_zero = true;
            } else {
                any_nonzero = true;
            }
        }
        const double n = static_cast<double>(m.arc_count);
        m.min_weight  = lo;
        m.max_weight  = hi;
        m.mean_weight = sum / n;
        // Variance via the raw second moment. Clamped at zero because
        // catastrophic cancellation can make the difference slightly negative
        // when every weight is identical.
        m.stddev_weight = std::sqrt(std::max(0.0, sum_sq / n - m.mean_weight * m.mean_weight));
        m.all_weights_zero     = !any_nonzero;
        m.all_weights_positive = !any_zero;
    }

    // --- Degrees -------------------------------------------------------------
    if (m.node_count > 0) {
        std::size_t lo  = std::numeric_limits<std::size_t>::max();
        std::size_t hi  = 0;
        double      sum = 0.0;
        double      sum_sq = 0.0;

        for (std::size_t u = 0; u < m.node_count; ++u) {
            const std::size_t d = g.out_degree(static_cast<NodeId>(u));
            lo = std::min(lo, d);
            hi = std::max(hi, d);
            const double dd = static_cast<double>(d);
            sum += dd;
            sum_sq += dd * dd;
            if (d == 0) {
                ++m.isolated_nodes;
            }
        }
        const double n    = static_cast<double>(m.node_count);
        m.min_out_degree  = lo;
        m.max_out_degree  = hi;
        m.mean_out_degree = sum / n;
        m.stddev_out_degree =
            std::sqrt(std::max(0.0, sum_sq / n - m.mean_out_degree * m.mean_out_degree));
    }

    return m;
}

}  // namespace lpq

#endif  // LPQ_GRAPH_GRAPH_METADATA_HPP
