// =============================================================================
// The result of a shortest-path computation.
//
// One type is returned by every algorithm and every queue in this project --
// the reference implementation, Dijkstra over each classical heap, A*, and LPQ
// in each of its modes. Differential testing (ESD.md section 24) compares these
// objects directly, which only works if every implementation populates the same
// fields with the same meaning.
//
// The comparison rules live in `differential.hpp` rather than here, because
// "these two results agree" is a policy question (exact cost equality for
// integers, tolerance for doubles, and predecessor arrays that may legitimately
// differ between equally optimal paths) rather than a property of the data.
// =============================================================================
#ifndef LPQ_ALGORITHMS_SHORTEST_PATH_RESULT_HPP
#define LPQ_ALGORITHMS_SHORTEST_PATH_RESULT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq {

/// Per-run priority-queue and traversal statistics (PRD.md section 20).
///
/// Populated only when instrumentation is compiled in (ESD.md section 19);
/// with LPQ_ENABLE_INSTRUMENTATION=0 the counters stay zero and the updating
/// code compiles away entirely, so the timed hot path carries no counter
/// increments at all.
struct SearchCounters {
    std::uint64_t pq_pushes        = 0;
    std::uint64_t pq_pops          = 0;
    std::uint64_t pq_decrease_keys = 0;
    /// Key comparisons performed by the queue. The primary machine-independent
    /// measure of queue work, and the one that makes heap implementations
    /// comparable across machines.
    std::uint64_t pq_comparisons   = 0;
    /// Pops discarded because the entry was stale. Non-zero only for the
    /// lazy-deletion queues; see dijkstra.hpp for why both strategies exist.
    std::uint64_t stale_pops       = 0;

    /// Nodes finalised (settled). For exact Dijkstra this equals the number of
    /// reachable nodes the search visited before stopping.
    std::uint64_t settled_nodes    = 0;
    /// Arcs examined during relaxation.
    std::uint64_t relaxations      = 0;
    /// Relaxations that improved a tentative distance.
    std::uint64_t successful_relaxations = 0;
    /// Largest size the queue reached. Drives memory expectations and is a
    /// feature input for the learned predictor.
    std::uint64_t peak_queue_size  = 0;

    void reset() { *this = SearchCounters{}; }
};

/// Why a search stopped. Distinguishing these matters: a search that stopped
/// early because it reached its target has NOT computed final distances for
/// every node, and comparing its distance array against a full SSSP run would
/// produce spurious differences.
enum class SearchTermination : std::uint8_t {
    /// The queue drained. Every reachable node is settled and its distance final.
    kExhausted,
    /// Single-target mode reached and settled the target, then stopped.
    kTargetReached,
    /// Single-target mode drained the queue without reaching the target: the
    /// target is unreachable from the source.
    kTargetUnreachable,
};

[[nodiscard]] constexpr const char* to_string(SearchTermination t) noexcept {
    switch (t) {
        case SearchTermination::kExhausted:         return "exhausted";
        case SearchTermination::kTargetReached:     return "target_reached";
        case SearchTermination::kTargetUnreachable: return "target_unreachable";
    }
    return "unknown";
}

/// Distances, predecessors and statistics from one shortest-path run.
template <WeightType W>
struct ShortestPathResult {
    using DistanceT = DistanceType<W>;

    /// dist[v] is the cost of the shortest source->v path, or kUnreachable<W>.
    ///
    /// In single-target mode only entries for *settled* nodes are final; other
    /// entries hold whatever tentative value the search had reached when it
    /// stopped. `settled` records which is which.
    std::vector<DistanceT> dist;

    /// parent[v] is the predecessor of v on the recorded shortest path, or
    /// kInvalidNode for the source and for unreached nodes.
    ///
    /// When several shortest paths of equal cost exist, which one is recorded
    /// depends on arc visitation and tie-breaking order, so two correct
    /// implementations may legitimately disagree here while agreeing on every
    /// cost. Differential tests therefore validate the parent array by
    /// reconstructing the path and checking its cost, not by comparing arrays.
    std::vector<NodeId> parent;

    /// settled[v] is true once v's distance is final.
    std::vector<bool> settled;

    NodeId source = kInvalidNode;
    /// kInvalidNode in full single-source mode.
    NodeId target = kInvalidNode;

    SearchTermination termination = SearchTermination::kExhausted;
    SearchCounters    counters{};

    /// Wall-clock duration of the search proper, excluding setup and
    /// allocation. Measured with the monotonic clock (ESD.md section 19).
    std::uint64_t runtime_ns = 0;

    [[nodiscard]] std::size_t num_nodes() const noexcept { return dist.size(); }

    [[nodiscard]] bool reachable(NodeId v) const noexcept {
        return !is_unreachable<W>(dist[v]);
    }

    /// Cost of the shortest path to `v`, or kUnreachable<W>.
    [[nodiscard]] DistanceT distance(NodeId v) const noexcept { return dist[v]; }

    /// Reconstructs the source -> `v` path by walking the predecessor array.
    ///
    /// Returns an empty vector when `v` is unreachable. The returned path runs
    /// source-first. A malformed predecessor chain (a cycle, which would mean a
    /// bug) is detected by bounding the walk at the node count rather than
    /// looping forever -- a test that hangs is far worse than one that fails.
    [[nodiscard]] std::vector<NodeId> path_to(NodeId v) const {
        std::vector<NodeId> path;
        if (v >= dist.size() || !reachable(v)) {
            return path;
        }
        const std::size_t limit = dist.size() + 1U;
        NodeId            cur   = v;
        while (cur != kInvalidNode) {
            path.push_back(cur);
            if (path.size() > limit) {
                return {};  // Cycle in the predecessor chain: report no path.
            }
            if (cur == source) {
                break;
            }
            cur = parent[cur];
        }
        if (path.empty() || path.back() != source) {
            return {};  // Chain did not reach the source.
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    /// Sums the arc weights along `path`, verifying each hop exists in `g`.
    ///
    /// This is the independent check that makes a reported cost trustworthy: it
    /// recomputes the cost from the graph rather than trusting `dist`. Returns
    /// kUnreachable<W> when the path is empty or contains a non-existent hop.
    /// Parallel arcs are handled by taking the cheapest arc for each hop, which
    /// is what a shortest path would have used.
    [[nodiscard]] DistanceT verify_path_cost(const CsrGraph<W>& g,
                                             const std::vector<NodeId>& path) const {
        if (path.empty()) {
            return kUnreachable<W>;
        }
        if (path.size() == 1U) {
            return path[0] == source ? DistanceT{} : kUnreachable<W>;
        }
        DistanceT total{};
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            bool found = false;
            W    best{};
            for (const Arc<W>& a : g.out_arcs(path[i])) {
                if (a.target == path[i + 1]) {
                    if (!found || a.weight < best) {
                        best  = a.weight;
                        found = true;
                    }
                }
            }
            if (!found) {
                return kUnreachable<W>;  // Path uses an arc the graph lacks.
            }
            total = static_cast<DistanceT>(total + static_cast<DistanceT>(best));
        }
        return total;
    }

    /// Allocates and initialises the arrays for an `n`-node search.
    void initialise(std::size_t n, NodeId src) {
        dist.assign(n, kUnreachable<W>);
        parent.assign(n, kInvalidNode);
        settled.assign(n, false);
        source      = src;
        target      = kInvalidNode;
        termination = SearchTermination::kExhausted;
        counters.reset();
        runtime_ns = 0;
        if (src < n) {
            dist[src] = DistanceT{};
        }
    }
};

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_SHORTEST_PATH_RESULT_HPP
