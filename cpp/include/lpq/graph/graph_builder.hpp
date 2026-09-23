// =============================================================================
// Edge-list to CSR construction.
//
// This is the only sanctioned way to build a CsrGraph from untrusted input. It
// performs the validation the graph's invariants assume and applies the
// normalisation policies that the dataset pipeline (ESD.md section 26) and the
// fuzz corpus (ESD.md section 24) depend on.
//
// Construction is a counting sort, not a comparison sort: two linear passes
// over the edge list plus a prefix sum, O(n + m) time and one temporary counter
// array. A comparison sort of the edge list would be O(m log m) and, on the
// large synthetic graphs of PRD.md section 17, would dominate load time.
//
// Within each node's adjacency, arcs are ordered by (target, weight). This is
// not required for correctness, but it makes graph loading deterministic, makes
// two graphs built from differently ordered edge lists compare equal, and gives
// the differential tests a stable arc visitation order so that tie-breaking
// behaviour is reproducible across runs and platforms.
// =============================================================================
#ifndef LPQ_GRAPH_GRAPH_BUILDER_HPP
#define LPQ_GRAPH_GRAPH_BUILDER_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq {

/// An input edge, before CSR construction.
template <WeightType W>
struct InputEdge {
    NodeId source;
    NodeId target;
    W      weight;
};

/// What to do with parallel edges between the same ordered node pair.
enum class ParallelEdgePolicy : std::uint8_t {
    /// Keep every arc exactly as supplied. Faithful to the source dataset, and
    /// the default: collapsing arcs would change priority-queue operation
    /// counts, and those counts are a reported metric (PRD.md section 20).
    kKeep,
    /// Keep only the minimum-weight arc per (source, target) pair. Shortest-path
    /// results are unchanged -- a heavier parallel arc can never be on a
    /// shortest path -- but the graph gets smaller. Useful for stress tests.
    kKeepMinimum,
};

/// What to do with self-loops (u -> u).
enum class SelfLoopPolicy : std::uint8_t {
    /// Keep them. They are harmless for Dijkstra: relaxing u through itself can
    /// never lower dist[u] when weights are non-negative. Keeping them exercises
    /// that argument in the fuzz corpus.
    kKeep,
    /// Drop them during construction.
    kDrop,
};

/// Construction options.
struct GraphBuildOptions {
    Directedness       directedness   = Directedness::kDirected;
    ParallelEdgePolicy parallel_edges = ParallelEdgePolicy::kKeep;
    SelfLoopPolicy     self_loops     = SelfLoopPolicy::kKeep;
    CoordinateSystem   coord_system   = CoordinateSystem::kNone;
    std::string        dataset_id;

    /// Set when `edges` already contains BOTH directions of every undirected
    /// edge -- as it does when the arcs come from a CSR graph rather than from
    /// a source edge list.
    ///
    /// Without this, rebuilding an undirected graph from its own arcs has to
    /// choose between two wrong answers: declare it directed and lose the label
    /// (which doubles the reported edge count, since num_edges() halves
    /// arcs_.size() only for undirected graphs), or declare it undirected and
    /// have the builder pair every arc a second time. This option is the third
    /// option -- keep the label, skip the pairing.
    bool arcs_already_paired = false;
};

/// Builds CsrGraph instances from edge lists.
template <WeightType W>
class GraphBuilder {
public:
    using ArcT   = Arc<W>;
    using EdgeT  = InputEdge<W>;
    using GraphT = CsrGraph<W>;

    /// Builds a graph with `node_count` nodes from `edges`.
    ///
    /// For an undirected graph each input edge is materialised as two arcs; the
    /// caller supplies each edge once. `coordinates` must be empty or have
    /// exactly `node_count` entries.
    [[nodiscard]] static GraphT build(std::size_t node_count, std::vector<EdgeT> edges,
                                      const GraphBuildOptions& opts,
                                      std::vector<Coordinate>  coordinates = {}) {
        if (node_count > kMaxNodeCount) {
            throw InvalidGraphError("node count " + std::to_string(node_count) +
                                    " exceeds the maximum addressable by NodeId");
        }
        validate_edges(node_count, edges);

        if (opts.self_loops == SelfLoopPolicy::kDrop) {
            std::erase_if(edges, [](const EdgeT& e) { return e.source == e.target; });
        }

        // Materialise reverse arcs for undirected graphs. A self-loop is added
        // only once even in the undirected case: duplicating it would double its
        // degree contribution and corrupt the degree statistics.
        if (opts.directedness == Directedness::kUndirected && !opts.arcs_already_paired) {
            std::vector<EdgeT> both;
            both.reserve(edges.size() * 2U);
            for (const EdgeT& e : edges) {
                both.push_back(e);
                if (e.source != e.target) {
                    both.push_back(EdgeT{e.target, e.source, e.weight});
                }
            }
            edges = std::move(both);
        }

        auto [offsets, arcs] = to_csr(node_count, edges);

        if (opts.parallel_edges == ParallelEdgePolicy::kKeepMinimum) {
            collapse_parallel_arcs(offsets, arcs);
        }

        if (!coordinates.empty() && coordinates.size() != node_count) {
            throw InvalidGraphError("coordinate count does not match the node count");
        }

        return GraphT(std::move(offsets), std::move(arcs), opts.directedness,
                      std::move(coordinates), opts.coord_system, opts.dataset_id);
    }

private:
    static void validate_edges(std::size_t node_count, const std::vector<EdgeT>& edges) {
        for (const EdgeT& e : edges) {
            if (static_cast<std::size_t>(e.source) >= node_count) {
                throw InvalidGraphError("edge source " + std::to_string(e.source) +
                                        " is out of range for " + std::to_string(node_count) +
                                        " nodes");
            }
            if (static_cast<std::size_t>(e.target) >= node_count) {
                throw InvalidGraphError("edge target " + std::to_string(e.target) +
                                        " is out of range for " + std::to_string(node_count) +
                                        " nodes");
            }
            if constexpr (std::same_as<W, double>) {
                if (!(e.weight >= 0.0)) {
                    throw InvalidGraphError("edge weight is negative or NaN");
                }
                if (e.weight == std::numeric_limits<double>::infinity()) {
                    throw InvalidGraphError("edge weight is infinite");
                }
            }
            // Unsigned integer weights cannot be negative by construction, which
            // is precisely why WeightType excludes signed integers.
        }
    }

    /// Counting sort of the edge list into CSR form.
    static std::pair<std::vector<EdgeId>, std::vector<ArcT>> to_csr(
        std::size_t node_count, const std::vector<EdgeT>& edges) {
        // The zero-node case is handled up front rather than falling through
        // the three passes below. Every pass is a no-op for it, but stating the
        // invariant here also lets the optimizer see that `offsets` has at
        // least two elements whenever the sort loop runs -- without it, GCC
        // constant-propagates node_count == 0 into the loop body and reports a
        // false-positive -Warray-bounds on offsets[u + 1].
        if (node_count == 0) {
            return {std::vector<EdgeId>{0U}, std::vector<ArcT>{}};
        }

        std::vector<EdgeId> offsets(node_count + 1U, 0U);

        // Pass 1: out-degree histogram, written one slot right so that the
        // prefix sum lands directly in the final offsets array.
        for (const EdgeT& e : edges) {
            ++offsets[static_cast<std::size_t>(e.source) + 1U];
        }
        for (std::size_t i = 1; i < offsets.size(); ++i) {
            offsets[i] += offsets[i - 1U];
        }

        // Pass 2: scatter. `cursor` tracks the next free slot per node.
        std::vector<ArcT>   arcs(edges.size());
        std::vector<EdgeId> cursor(offsets.begin(), offsets.end() - 1);
        for (const EdgeT& e : edges) {
            arcs[cursor[e.source]++] = ArcT{e.target, e.weight};
        }

        // Canonical intra-adjacency order, for deterministic traversal.
        for (std::size_t u = 0; u < node_count; ++u) {
            const auto b = static_cast<std::ptrdiff_t>(offsets[u]);
            const auto e = static_cast<std::ptrdiff_t>(offsets[u + 1U]);
            std::sort(arcs.begin() + b, arcs.begin() + e, [](const ArcT& l, const ArcT& r) {
                if (l.target != r.target) {
                    return l.target < r.target;
                }
                return l.weight < r.weight;
            });
        }
        return {std::move(offsets), std::move(arcs)};
    }

    /// Keeps only the minimum-weight arc per (source, target) pair.
    ///
    /// Relies on the canonical order established above: parallel arcs are
    /// adjacent and the lightest comes first, so a single linear pass suffices.
    static void collapse_parallel_arcs(std::vector<EdgeId>& offsets, std::vector<ArcT>& arcs) {
        const std::size_t node_count = offsets.size() - 1U;
        std::vector<ArcT> kept;
        kept.reserve(arcs.size());
        std::vector<EdgeId> new_offsets(offsets.size(), 0U);

        for (std::size_t u = 0; u < node_count; ++u) {
            new_offsets[u] = static_cast<EdgeId>(kept.size());
            const EdgeId b = offsets[u];
            const EdgeId e = offsets[u + 1U];
            for (EdgeId i = b; i < e; ++i) {
                if (i == b || arcs[i].target != arcs[i - 1U].target) {
                    kept.push_back(arcs[i]);
                }
            }
        }
        new_offsets[node_count] = static_cast<EdgeId>(kept.size());
        offsets                 = std::move(new_offsets);
        arcs                    = std::move(kept);
    }
};

}  // namespace lpq

#endif  // LPQ_GRAPH_GRAPH_BUILDER_HPP
