// =============================================================================
// A* over a pluggable priority queue and heuristic (ESD.md section 9).
//
// Structurally Dijkstra with the queue keyed on f = g + h instead of g. The
// differences that matter are all consequences of that one change, and each is
// a place where a plausible-looking implementation is quietly wrong.
//
// -----------------------------------------------------------------------------
// 1. dist[] stores g, the queue stores f
// -----------------------------------------------------------------------------
//
// The distance array must hold the true path cost g, not the key f. Storing f
// would make the returned cost include the heuristic estimate, so the result
// would be neither the path cost nor comparable with Dijkstra's -- and, worse,
// the relaxation test `g[u] + w < g[v]` would be comparing quantities that
// include different heuristic terms. The key is reconstructed as g + h(v) at
// push time and is never read back as a distance.
//
// -----------------------------------------------------------------------------
// 2. Early exit is on EXTRACTION, never on discovery
// -----------------------------------------------------------------------------
//
// A* may stop when the target is POPPED, not when it is first reached by a
// relaxation. At discovery the target's tentative g is merely an upper bound;
// a cheaper route may still be in the queue. Stopping at discovery is the most
// common A* bug and yields paths that are usually optimal, which is precisely
// what makes it hard to catch -- the differential tests here run enough queries
// to find the cases where "usually" fails.
//
// -----------------------------------------------------------------------------
// 3. Consistency decides whether re-expansion is needed
// -----------------------------------------------------------------------------
//
// With a CONSISTENT heuristic, f is non-decreasing along the search, a node's
// first extraction is final, and settled nodes are never revisited -- exactly
// Dijkstra's argument.
//
// With a merely ADMISSIBLE heuristic, f can decrease, so a node can be
// extracted with a g that is later improved. Correctness then requires allowing
// re-expansion of an already-settled node when a strictly better g is found.
// The two are different algorithms with different operation counts, so the
// choice is made at compile time from the heuristic's own declaration rather
// than guessed at, and is recorded in the result.
//
// Every heuristic currently shipped is consistent. The re-expansion path exists
// because the learned-heuristic experiment will not be.
//
// -----------------------------------------------------------------------------
// 4. The radix heap needs a consistent heuristic
// -----------------------------------------------------------------------------
//
// Monotone keys are its correctness precondition. An admissible-but-
// inconsistent heuristic can push an f below the last extracted key, landing
// the entry in a drained bucket where it is silently lost. That is a wrong
// answer rather than a crash, so the combination is rejected at compile time
// rather than trusted.
// =============================================================================
#ifndef LPQ_ALGORITHMS_ASTAR_HPP
#define LPQ_ALGORITHMS_ASTAR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lpq/algorithms/heuristics.hpp"
#include "lpq/algorithms/shortest_path_result.hpp"
#include "lpq/common/clock.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/numeric.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

/// Options for an A* run.
struct AstarOptions {
    /// A* is inherently point-to-point; a target is required.
    NodeId target = kInvalidNode;
    bool   measure_time = true;
};

/// Runs A* from `source` to `opts.target`.
///
/// The returned `dist` array holds true path costs (g), directly comparable
/// with Dijkstra's. Only settled entries are final.
template <WeightType W, PriorityQueueLike Queue, typename Heuristic>
[[nodiscard]] ShortestPathResult<W> astar(const CsrGraph<W>& g, NodeId source, Queue& queue,
                                          Heuristic& h, const AstarOptions& opts = {}) {
    using DistanceT = DistanceType<W>;
    static_assert(std::is_same_v<typename Queue::KeyType, DistanceT>,
                  "queue key type must match the graph's distance type");

    // A non-consistent heuristic can lower f below the last extracted key,
    // which silently loses entries in a radix heap. Rejected here rather than
    // producing invalid results (ESD.md section 7).
    static_assert(Queue::kSupportsDecreaseKey || Heuristic::kIsConsistent,
                  "a lazy-deletion monotone queue (such as the radix heap) requires a "
                  "consistent heuristic; f may otherwise decrease between extractions");

    ShortestPathResult<W> r;
    const std::size_t     n = g.num_nodes();
    r.initialise(n, source);
    r.target = opts.target;

    if (opts.target == kInvalidNode) {
        throw InvalidConfigError("astar requires a target; use dijkstra for full SSSP");
    }
    if (n == 0 || source >= n || opts.target >= n) {
        r.termination = SearchTermination::kTargetUnreachable;
        return r;
    }

    queue.clear();
    queue.reset_comparison_count();
    h.prepare(g, opts.target);

    const TimePoint start = opts.measure_time ? now() : TimePoint{};

    queue.push(source, h.estimate(source));
    count_event(r.counters.pq_pushes);

    while (!queue.empty()) {
        if constexpr (kInstrumentationEnabled) {
            r.counters.peak_queue_size = std::max(r.counters.peak_queue_size,
                                                  static_cast<std::uint64_t>(queue.size()));
        }

        const QueueEntry<DistanceT> entry = queue.pop();
        count_event(r.counters.pq_pops);
        const NodeId u = entry.node;

        if constexpr (!Queue::kSupportsDecreaseKey) {
            // Lazy deletion. The stale test is on g, not on the popped key:
            // the key is f and carries the heuristic term, so comparing it
            // against dist[] -- which holds g -- would be comparing unlike
            // quantities.
            if constexpr (Heuristic::kIsConsistent) {
                if (r.settled[u]) {
                    count_event(r.counters.stale_pops);
                    continue;
                }
            }
            // With an inconsistent heuristic a settled node may legitimately be
            // re-expanded, so only genuinely superseded entries are dropped.
            const DistanceT g_at_pop =
                entry.key >= h.estimate(u) ? static_cast<DistanceT>(entry.key - h.estimate(u))
                                           : DistanceT{};
            if (g_at_pop > r.dist[u]) {
                count_event(r.counters.stale_pops);
                continue;
            }
        } else {
            if constexpr (Heuristic::kIsConsistent) {
                // First extraction is final; a decrease-key queue holds one
                // entry per node, so this can only fire via re-insertion.
                if (r.settled[u]) {
                    count_event(r.counters.stale_pops);
                    continue;
                }
            }
        }

        r.settled[u] = true;
        count_event(r.counters.settled_nodes);

        // Early exit on EXTRACTION. See the header comment: stopping at
        // discovery yields paths that are usually, but not always, optimal.
        if (u == opts.target) {
            r.termination = SearchTermination::kTargetReached;
            if (opts.measure_time) {
                r.runtime_ns = elapsed_ns_since(start);
            }
            if constexpr (kInstrumentationEnabled) {
                r.counters.pq_comparisons = queue.comparison_count();
            }
            return r;
        }

        const DistanceT gu = r.dist[u];
        for (const Arc<W>& a : g.out_arcs(u)) {
            count_event(r.counters.relaxations);
            const DistanceT candidate = saturating_add<W>(gu, a.weight);
            if (candidate < r.dist[a.target]) {
                r.dist[a.target]   = candidate;
                r.parent[a.target] = u;
                count_event(r.counters.successful_relaxations);

                // With an inconsistent heuristic an improved g may reopen a
                // settled node. Skipped entirely for consistent heuristics,
                // where it can never happen.
                if constexpr (!Heuristic::kIsConsistent) {
                    r.settled[a.target] = false;
                }

                const DistanceT key = saturating_add_distances<W>(candidate, h.estimate(a.target));
                if constexpr (Queue::kSupportsDecreaseKey) {
                    queue.decrease_key(a.target, key);
                    count_event(r.counters.pq_decrease_keys);
                } else {
                    queue.push(a.target, key);
                    count_event(r.counters.pq_pushes);
                }
            }
        }
    }

    r.termination = SearchTermination::kTargetUnreachable;
    if (opts.measure_time) {
        r.runtime_ns = elapsed_ns_since(start);
    }
    if constexpr (kInstrumentationEnabled) {
        r.counters.pq_comparisons = queue.comparison_count();
    }
    return r;
}

/// Convenience overload constructing the queue and heuristic.
///
/// For tests and one-off use; benchmarks must use the reference-taking overload
/// so allocation stays outside the timed region.
template <WeightType W, PriorityQueueLike Queue, typename Heuristic>
[[nodiscard]] ShortestPathResult<W> astar(const CsrGraph<W>& g, NodeId source,
                                          const AstarOptions& opts = {},
                                          Heuristic h = Heuristic{}) {
    Queue queue;
    queue.reserve_nodes(g.num_nodes());
    return astar<W, Queue, Heuristic>(g, source, queue, h, opts);
}

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_ASTAR_HPP
