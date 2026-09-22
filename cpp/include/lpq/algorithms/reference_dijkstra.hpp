// =============================================================================
// Trusted reference implementation of Dijkstra (ESD.md section 4.1).
//
// This is the correctness anchor for the entire project. Every other
// implementation -- Dijkstra over each classical heap, A*, and LPQ in exact and
// approximate modes -- is validated against it by the differential tests.
//
// DESIGN: this implementation deliberately uses a linear scan over an array to
// find the minimum, giving O(n^2 + m) rather than O((n + m) log n).
//
// That is not an oversight. A reference implementation must fail *independently*
// of the code it validates. If the reference used a binary heap, then a bug in
// that heap would corrupt the reference and the implementation under test
// identically, the differential test would compare two equally wrong answers,
// and it would pass. The linear scan shares no code, no data structure and no
// invariant with any priority queue in this project, so a queue bug cannot hide
// inside it.
//
// The cost of that independence is that the reference is only usable on small
// graphs. That is exactly how it is used: Tier 0 correctness testing (ESD.md
// section 33) and the fuzz corpus run on graphs of tens to a few thousand
// nodes. For validating large benchmark queries, `reference_dijkstra_heap` --
// declared below and implemented with std::priority_queue from the standard
// library, which is again code this project does not own -- provides a second
// independent oracle that scales.
//
// Clarity is preferred over speed throughout. This code is never benchmarked
// and never appears in a timed region.
// =============================================================================
#ifndef LPQ_ALGORITHMS_REFERENCE_DIJKSTRA_HPP
#define LPQ_ALGORITHMS_REFERENCE_DIJKSTRA_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "lpq/algorithms/shortest_path_result.hpp"
#include "lpq/common/numeric.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq {

/// Options controlling a reference run.
struct ReferenceDijkstraOptions {
    /// Stop once this node is settled. kInvalidNode runs a full single-source
    /// search over every reachable node.
    NodeId target = kInvalidNode;
};

// -----------------------------------------------------------------------------
// O(n^2) reference
// -----------------------------------------------------------------------------

/// Computes shortest paths from `source` by repeated linear minimum scan.
///
/// Preconditions: all arc weights non-negative (guaranteed by the WeightType
/// concept for integers and validated at load time for doubles).
///
/// Tie-breaking: among nodes of equal tentative distance, the one with the
/// smallest node id is settled first. This is stated explicitly because the
/// choice is observable in the predecessor array, and the differential tests
/// need a deterministic reference to describe disagreements against.
template <WeightType W>
[[nodiscard]] ShortestPathResult<W> reference_dijkstra(
    const CsrGraph<W>& g, NodeId source,
    const ReferenceDijkstraOptions& opts = {}) {
    using DistanceT = DistanceType<W>;

    ShortestPathResult<W> r;
    const std::size_t     n = g.num_nodes();
    r.initialise(n, source);
    r.target = opts.target;

    if (n == 0 || source >= n) {
        r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                      : SearchTermination::kTargetUnreachable;
        return r;
    }

    for (std::size_t iteration = 0; iteration < n; ++iteration) {
        // --- Select the unsettled node of minimum tentative distance ---------
        //
        // Scanning every node every iteration is what makes this O(n^2). The
        // strict `<` combined with ascending iteration order is what implements
        // the smallest-node-id tie-break documented above.
        NodeId    best      = kInvalidNode;
        DistanceT best_dist = kUnreachable<W>;
        for (std::size_t v = 0; v < n; ++v) {
            if (r.settled[v]) {
                continue;
            }
            ++r.counters.pq_comparisons;
            if (r.dist[v] < best_dist) {
                best_dist = r.dist[v];
                best      = static_cast<NodeId>(v);
            }
        }

        // No reachable unsettled node remains.
        if (best == kInvalidNode || is_unreachable<W>(best_dist)) {
            r.termination = (opts.target == kInvalidNode)
                                ? SearchTermination::kExhausted
                                : SearchTermination::kTargetUnreachable;
            return r;
        }

        r.settled[best] = true;
        ++r.counters.settled_nodes;
        ++r.counters.pq_pops;

        if (opts.target != kInvalidNode && best == opts.target) {
            r.termination = SearchTermination::kTargetReached;
            return r;
        }

        // --- Relax outgoing arcs ---------------------------------------------
        for (const Arc<W>& a : g.out_arcs(best)) {
            ++r.counters.relaxations;
            // best_dist is finite here, so saturating addition only matters for
            // pathological weight sums; it is used anyway so that a wrap can
            // never invent a shorter path.
            const DistanceT candidate = saturating_add<W>(best_dist, a.weight);
            if (candidate < r.dist[a.target]) {
                r.dist[a.target]   = candidate;
                r.parent[a.target] = best;
                ++r.counters.successful_relaxations;
            }
        }
    }

    r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                  : SearchTermination::kTargetUnreachable;
    return r;
}

// -----------------------------------------------------------------------------
// Second, independent oracle that scales
// -----------------------------------------------------------------------------

/// Computes shortest paths using `std::priority_queue` with lazy deletion.
///
/// A second oracle for graphs too large for the O(n^2) reference. Its
/// independence comes from using the standard library's heap rather than any
/// queue implemented in this project: a bug in `lpq::BinaryHeap` cannot affect
/// it. It is weaker evidence than the linear-scan reference, since it shares
/// the general binary-heap *idea*, so the two are used in layers -- the O(n^2)
/// reference on every small fuzz graph, this one on large benchmark queries.
///
/// Tie-breaking differs from the linear-scan reference (std::priority_queue
/// makes no ordering promise among equal keys), so the two may record different
/// predecessor arrays for the same graph. They must always agree on costs, and
/// that is what the differential tests assert.
template <WeightType W>
[[nodiscard]] ShortestPathResult<W> reference_dijkstra_heap(
    const CsrGraph<W>& g, NodeId source,
    const ReferenceDijkstraOptions& opts = {}) {
    using DistanceT = DistanceType<W>;
    using Entry     = std::pair<DistanceT, NodeId>;  // (key, node)

    ShortestPathResult<W> r;
    const std::size_t     n = g.num_nodes();
    r.initialise(n, source);
    r.target = opts.target;

    if (n == 0 || source >= n) {
        r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                      : SearchTermination::kTargetUnreachable;
        return r;
    }

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;
    pq.emplace(DistanceT{}, source);
    ++r.counters.pq_pushes;

    while (!pq.empty()) {
        r.counters.peak_queue_size =
            std::max(r.counters.peak_queue_size, static_cast<std::uint64_t>(pq.size()));

        const auto [d, u] = pq.top();
        pq.pop();
        ++r.counters.pq_pops;

        // Lazy deletion: an entry is stale if a better distance was recorded
        // after it was pushed. Skipping stale entries is what makes lazy
        // deletion correct without a decrease-key operation.
        if (r.settled[u]) {
            ++r.counters.stale_pops;
            continue;
        }
        if (d > r.dist[u]) {
            ++r.counters.stale_pops;
            continue;
        }

        r.settled[u] = true;
        ++r.counters.settled_nodes;

        if (opts.target != kInvalidNode && u == opts.target) {
            r.termination = SearchTermination::kTargetReached;
            return r;
        }

        for (const Arc<W>& a : g.out_arcs(u)) {
            ++r.counters.relaxations;
            const DistanceT candidate = saturating_add<W>(d, a.weight);
            if (candidate < r.dist[a.target]) {
                r.dist[a.target]   = candidate;
                r.parent[a.target] = u;
                ++r.counters.successful_relaxations;
                pq.emplace(candidate, a.target);
                ++r.counters.pq_pushes;
            }
        }
    }

    r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                  : SearchTermination::kTargetUnreachable;
    return r;
}

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_REFERENCE_DIJKSTRA_HPP
