// =============================================================================
// Per-node static features, precomputed once per graph (ESD.md section 11).
//
// ESD.md section 11 is explicit: "Feature extraction must not perform
// graph-wide scans." Several features PRD.md section 9 asks for -- node degree,
// and the minimum, mean and maximum adjacent edge weight -- are properties of a
// node's whole adjacency. Computing them during extraction would mean a scan
// per candidate, which on a high-degree node is exactly the cost the learned
// queue is supposed to save.
//
// They are therefore computed ONCE, in a single O(n + m) pass at load time, and
// read back as an array lookup during the search. The precomputation is not in
// any timed region; the lookup is one cache line.
//
// -----------------------------------------------------------------------------
// Layout: struct-of-arrays, and already normalised
// -----------------------------------------------------------------------------
//
// Values are stored as float, pre-normalised against the graph's own
// statistics, and interleaved into one record per node.
//
// Interleaved rather than parallel arrays because extraction reads ALL of a
// node's static features together, exactly as the CSR arc array interleaves
// target and weight for the same reason: one stream and one cache line instead
// of six.
//
// Pre-normalising means the division happens once per node at load rather than
// once per node per extraction -- and a node is typically extracted and
// inspected many times across a query set.
// =============================================================================
#ifndef LPQ_FEATURES_GRAPH_FEATURES_HPP
#define LPQ_FEATURES_GRAPH_FEATURES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_metadata.hpp"

namespace lpq {

/// Static, normalised features for one node.
///
/// 24 bytes, so two nodes share a 64-byte cache line where the allocator
/// permits. Kept deliberately small: this array is touched once per candidate
/// evaluation and competes with the CSR arrays for cache.
struct NodeStaticFeatures {
    float out_degree            = 0.0F;  ///< normalised by the graph's max degree
    float min_adjacent_weight   = 0.0F;  ///< normalised by the graph's max weight
    float mean_adjacent_weight  = 0.0F;
    float max_adjacent_weight   = 0.0F;
    float weight_spread         = 0.0F;  ///< (max - min) / max_weight
    float degree_relative_to_mean = 0.0F;  ///< degree / mean_degree
};

static_assert(sizeof(NodeStaticFeatures) == 24,
              "NodeStaticFeatures is expected to stay compact; growing it increases "
              "cache pressure on the hottest feature path");

/// Graph-level constants used to normalise dynamic features during extraction.
///
/// Derived from the graph alone, never from queries or outcomes, so they are
/// legitimately available before a test query runs (ESD.md section 30).
struct GraphFeatureNormalisation {
    /// Reciprocals are stored so extraction multiplies rather than divides;
    /// a division is an order of magnitude slower and this happens per
    /// candidate.
    float inv_max_weight      = 1.0F;
    float inv_mean_weight     = 1.0F;
    float inv_max_degree      = 1.0F;
    float inv_mean_degree     = 1.0F;
    float inv_node_count      = 1.0F;
    /// A loose upper bound on any path cost, used to normalise distances.
    /// Deliberately the loose (n-1) * max_weight bound: it is guaranteed
    /// correct for any graph, and a normaliser only needs to put values on a
    /// comparable scale, not to be tight.
    float inv_max_path_cost   = 1.0F;

    // Graph-context features, already normalised and constant per graph.
    float mean_degree         = 0.0F;
    float degree_stddev       = 0.0F;
    float mean_weight         = 0.0F;
};

/// Static features for every node, plus the normalisation constants.
class GraphFeatures {
public:
    GraphFeatures() = default;

    /// Computes everything in one pass over the CSR arrays.
    ///
    /// O(n + m). Called once per graph at load time.
    template <WeightType W>
    explicit GraphFeatures(const CsrGraph<W>& g, const GraphMetadata& meta) {
        build(g, meta);
    }

    template <WeightType W>
    void build(const CsrGraph<W>& g, const GraphMetadata& meta) {
        const std::size_t n = g.num_nodes();
        nodes_.assign(n, NodeStaticFeatures{});

        // --- Normalisation constants ----------------------------------------
        //
        // Every denominator is guarded: an empty graph, an all-zero-weight
        // graph and a graph of isolated nodes are all legal inputs that the
        // fuzz corpus generates, and each makes one of these zero. A division
        // by zero here would produce infinities that propagate silently into
        // the model instead of failing.
        auto safe_inv = [](double x) {
            return x > 0.0 ? static_cast<float>(1.0 / x) : 0.0F;
        };

        norm_.inv_max_weight    = safe_inv(meta.max_weight);
        norm_.inv_mean_weight   = safe_inv(meta.mean_weight);
        norm_.inv_max_degree    = safe_inv(static_cast<double>(meta.max_out_degree));
        norm_.inv_mean_degree   = safe_inv(meta.mean_out_degree);
        norm_.inv_node_count    = safe_inv(static_cast<double>(meta.node_count));
        norm_.inv_max_path_cost = safe_inv(meta.max_path_cost_bound());

        norm_.mean_degree   = static_cast<float>(meta.mean_out_degree);
        norm_.degree_stddev = static_cast<float>(meta.stddev_out_degree);
        norm_.mean_weight   = static_cast<float>(meta.mean_weight) * norm_.inv_max_weight;

        // --- Per-node pass ---------------------------------------------------
        for (std::size_t u = 0; u < n; ++u) {
            const auto arcs = g.out_arcs(static_cast<NodeId>(u));
            NodeStaticFeatures f;

            f.out_degree = static_cast<float>(arcs.size()) * norm_.inv_max_degree;
            f.degree_relative_to_mean =
                static_cast<float>(arcs.size()) * norm_.inv_mean_degree;

            if (!arcs.empty()) {
                double lo  = static_cast<double>(arcs[0].weight);
                double hi  = lo;
                double sum = 0.0;
                for (const Arc<W>& a : arcs) {
                    const double w = static_cast<double>(a.weight);
                    lo  = std::min(lo, w);
                    hi  = std::max(hi, w);
                    sum += w;
                }
                const double mean = sum / static_cast<double>(arcs.size());
                f.min_adjacent_weight  = static_cast<float>(lo) * norm_.inv_max_weight;
                f.mean_adjacent_weight = static_cast<float>(mean) * norm_.inv_max_weight;
                f.max_adjacent_weight  = static_cast<float>(hi) * norm_.inv_max_weight;
                f.weight_spread        = static_cast<float>(hi - lo) * norm_.inv_max_weight;
            }
            // An isolated node keeps all-zero weight features, which is the
            // honest encoding: it has no adjacent weights, and zero is
            // distinguishable from any real normalised weight only in that it
            // co-occurs with a zero degree.

            nodes_[u] = f;
        }
    }

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    [[nodiscard]] const NodeStaticFeatures& node(NodeId u) const noexcept { return nodes_[u]; }

    [[nodiscard]] const GraphFeatureNormalisation& normalisation() const noexcept {
        return norm_;
    }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return nodes_.capacity() * sizeof(NodeStaticFeatures);
    }

private:
    std::vector<NodeStaticFeatures> nodes_;
    GraphFeatureNormalisation       norm_;
};

}  // namespace lpq

#endif  // LPQ_FEATURES_GRAPH_FEATURES_HPP
