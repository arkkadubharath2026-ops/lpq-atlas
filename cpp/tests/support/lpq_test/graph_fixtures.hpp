// =============================================================================
// Shared graph fixtures and random graph generation for the test suite.
//
// ESD.md section 24 names the edge cases the fuzz corpus must contain:
// duplicate edges, zero-weight edges, disconnected graphs, high-degree nodes,
// extremely uneven weights, ties, and tiny graphs. Each has a named builder
// here so that a failing test says which pathology it was exercising rather
// than printing an anonymous random graph.
//
// Everything is seeded. A failing randomised test reports its seed and can be
// replayed exactly.
// =============================================================================
#ifndef LPQ_TEST_GRAPH_FIXTURES_HPP
#define LPQ_TEST_GRAPH_FIXTURES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_builder.hpp"

namespace lpq_test {

using lpq::Arc;
using lpq::Coordinate;
using lpq::CoordinateSystem;
using lpq::CsrGraph;
using lpq::Directedness;
using lpq::EdgeId;
using lpq::GraphBuilder;
using lpq::GraphBuildOptions;
using lpq::InputEdge;
using lpq::NodeId;
using lpq::Rng;

// -----------------------------------------------------------------------------
// Deterministic hand-built graphs
// -----------------------------------------------------------------------------

/// The canonical worked example used across the unit tests.
///
///     0 --1--> 1 --2--> 3
///     |        |        ^
///     4        1        |
///     v        v        3
///     2 <--1-- 1        |
///     +----------------- +
///
/// Concretely (directed, uint32 weights):
///   0->1 (1), 0->2 (4), 1->2 (1), 1->3 (2), 2->3 (3)
///
/// Shortest distances from node 0: [0, 1, 2, 3].
/// Note that 0->2 direct costs 4 but 0->1->2 costs 2, so the graph exercises a
/// genuine decrease-key / improved-distance event rather than a trivial scan.
[[nodiscard]] inline CsrGraph<std::uint32_t> diamond_graph() {
    std::vector<InputEdge<std::uint32_t>> edges{
        {0, 1, 1}, {0, 2, 4}, {1, 2, 1}, {1, 3, 2}, {2, 3, 3},
    };
    GraphBuildOptions opts;
    opts.directedness = Directedness::kDirected;
    opts.dataset_id   = "fixture:diamond";
    return GraphBuilder<std::uint32_t>::build(4, std::move(edges), opts);
}

/// Expected single-source distances from node 0 of `diamond_graph()`.
[[nodiscard]] inline std::vector<std::uint64_t> diamond_expected_distances() {
    return {0, 1, 2, 3};
}

/// A path graph 0 -> 1 -> ... -> (n-1), every arc of weight 1.
///
/// Distances from 0 are exactly the node index, which makes it the cheapest
/// possible assertion of end-to-end correctness at any scale.
template <lpq::WeightType W = std::uint32_t>
[[nodiscard]] CsrGraph<W> path_graph(std::size_t n, W weight = W{1}) {
    std::vector<InputEdge<W>> edges;
    edges.reserve(n == 0 ? 0 : n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        edges.push_back({static_cast<NodeId>(i), static_cast<NodeId>(i + 1), weight});
    }
    GraphBuildOptions opts;
    opts.dataset_id = "fixture:path";
    return GraphBuilder<W>::build(n, std::move(edges), opts);
}

/// Two disjoint path components, so that nodes in the second are unreachable
/// from a source in the first.
template <lpq::WeightType W = std::uint32_t>
[[nodiscard]] CsrGraph<W> disconnected_graph(std::size_t per_component = 3) {
    std::vector<InputEdge<W>> edges;
    const auto                n = per_component * 2;
    for (std::size_t i = 0; i + 1 < per_component; ++i) {
        edges.push_back({static_cast<NodeId>(i), static_cast<NodeId>(i + 1), W{1}});
    }
    for (std::size_t i = per_component; i + 1 < n; ++i) {
        edges.push_back({static_cast<NodeId>(i), static_cast<NodeId>(i + 1), W{1}});
    }
    GraphBuildOptions opts;
    opts.dataset_id = "fixture:disconnected";
    return GraphBuilder<W>::build(n, std::move(edges), opts);
}

/// A star: node 0 connected to every other node. Exercises a single very
/// high-degree node against many degree-one nodes.
template <lpq::WeightType W = std::uint32_t>
[[nodiscard]] CsrGraph<W> star_graph(std::size_t n, W weight = W{1}) {
    std::vector<InputEdge<W>> edges;
    edges.reserve(n == 0 ? 0 : n - 1);
    for (std::size_t i = 1; i < n; ++i) {
        edges.push_back({0, static_cast<NodeId>(i), weight});
    }
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    opts.dataset_id   = "fixture:star";
    return GraphBuilder<W>::build(n, std::move(edges), opts);
}

/// Every arc has weight zero: every reachable node ties at distance zero.
/// The hardest case for tie-breaking policy.
template <lpq::WeightType W = std::uint32_t>
[[nodiscard]] CsrGraph<W> zero_weight_graph(std::size_t n) {
    return path_graph<W>(n, W{0});
}

/// Parallel arcs of differing weight between the same pair, plus a self-loop.
[[nodiscard]] inline CsrGraph<std::uint32_t> duplicate_edge_graph() {
    std::vector<InputEdge<std::uint32_t>> edges{
        {0, 1, 5}, {0, 1, 2}, {0, 1, 9},  // three parallel arcs; only weight 2 matters
        {1, 1, 3},                        // self-loop, must never improve dist[1]
        {1, 2, 1}, {1, 2, 1},             // exact duplicates, including the weight
    };
    GraphBuildOptions opts;
    opts.dataset_id = "fixture:duplicates";
    return GraphBuilder<std::uint32_t>::build(3, std::move(edges), opts);
}

/// A 4-connected grid with unit weights, optionally carrying the Euclidean
/// coordinates that the A* heuristics need.
template <lpq::WeightType W = std::uint32_t>
[[nodiscard]] CsrGraph<W> grid_graph(std::size_t rows, std::size_t cols, W weight = W{1},
                                     bool with_coordinates = true) {
    const std::size_t         n = rows * cols;
    std::vector<InputEdge<W>> edges;
    std::vector<Coordinate>   coords;
    if (with_coordinates) {
        coords.resize(n);
    }
    auto id = [cols](std::size_t r, std::size_t c) { return static_cast<NodeId>(r * cols + c); };

    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            if (with_coordinates) {
                coords[id(r, c)] = Coordinate{static_cast<double>(c), static_cast<double>(r)};
            }
            if (c + 1 < cols) {
                edges.push_back({id(r, c), id(r, c + 1), weight});
            }
            if (r + 1 < rows) {
                edges.push_back({id(r, c), id(r + 1, c), weight});
            }
        }
    }
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    opts.coord_system = with_coordinates ? CoordinateSystem::kEuclidean : CoordinateSystem::kNone;
    opts.dataset_id   = "fixture:grid";
    return GraphBuilder<W>::build(n, std::move(edges), opts, std::move(coords));
}

// -----------------------------------------------------------------------------
// Randomised graphs for differential and fuzz testing
// -----------------------------------------------------------------------------

/// Knobs for the fuzz generator. Defaults produce a small, well-behaved graph;
/// individual tests turn on the pathologies they mean to exercise.
struct RandomGraphSpec {
    std::size_t  min_nodes = 1;
    std::size_t  max_nodes = 32;
    double       edge_probability = 0.15;  ///< Per ordered pair.
    std::uint32_t min_weight = 1;
    std::uint32_t max_weight = 100;
    bool         allow_self_loops = true;
    bool         allow_parallel_edges = true;
    bool         allow_zero_weights = false;
    /// Draws weights from {1, max_weight} only, creating an extreme spread that
    /// stresses radix-heap bucketing and floating-point accumulation.
    bool         extreme_weight_spread = false;
    /// Forces many arcs to share a weight, maximising ties in the queue.
    bool         many_ties = false;
    Directedness directedness = Directedness::kDirected;
};

/// Generates a random graph from `spec` using `rng`.
///
/// Edges are drawn per ordered pair, so the expected arc count is
/// n * (n - 1) * edge_probability. Node counts stay small by default because
/// fuzz tests compare against an O(n^2) reference implementation.
[[nodiscard]] inline CsrGraph<std::uint32_t> random_graph(Rng& rng, const RandomGraphSpec& spec) {
    const std::size_t n = static_cast<std::size_t>(
        rng.next_in_range(spec.min_nodes, spec.max_nodes));

    std::vector<InputEdge<std::uint32_t>> edges;
    const std::uint32_t lo = spec.allow_zero_weights ? 0U : spec.min_weight;

    auto draw_weight = [&]() -> std::uint32_t {
        if (spec.many_ties) {
            // Only a handful of distinct values, so ties are the common case.
            static constexpr std::uint32_t kTieValues[] = {1, 1, 1, 2, 2, 7};
            return kTieValues[rng.next_below(6)];
        }
        if (spec.extreme_weight_spread) {
            return rng.next_unit() < 0.5 ? 1U : spec.max_weight;
        }
        return static_cast<std::uint32_t>(rng.next_in_range(lo, spec.max_weight));
    };

    for (std::size_t u = 0; u < n; ++u) {
        for (std::size_t v = 0; v < n; ++v) {
            if (u == v && !spec.allow_self_loops) {
                continue;
            }
            if (rng.next_unit() >= spec.edge_probability) {
                continue;
            }
            edges.push_back({static_cast<NodeId>(u), static_cast<NodeId>(v), draw_weight()});
            // Occasionally emit a second arc for the same pair.
            if (spec.allow_parallel_edges && rng.next_unit() < 0.1) {
                edges.push_back({static_cast<NodeId>(u), static_cast<NodeId>(v), draw_weight()});
            }
        }
    }

    GraphBuildOptions opts;
    opts.directedness = spec.directedness;
    opts.dataset_id   = "fuzz:seed" + std::to_string(rng.seed());
    return GraphBuilder<std::uint32_t>::build(n, std::move(edges), opts);
}

}  // namespace lpq_test

#endif  // LPQ_TEST_GRAPH_FIXTURES_HPP
