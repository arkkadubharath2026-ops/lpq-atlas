// =============================================================================
// Dijkstra over a pluggable priority queue (ESD.md section 8).
//
// One implementation serves every queue. The queue is a template parameter, so
// all of its calls are statically bound and inlinable; there is no virtual
// dispatch and no indirection that would tax the baselines (PRD.md section 24).
//
// -----------------------------------------------------------------------------
// Handling both key-update strategies from one code path
// -----------------------------------------------------------------------------
//
// ESD.md section 8 requires the decrease-key / lazy-deletion choice to be made
// consistently per queue and documented. The two strategies differ in exactly
// two places, and both are resolved at compile time via
// `Queue::kSupportsDecreaseKey`:
//
//   decrease-key   On an improved distance, call decrease_key. The queue holds
//                  one entry per node, so a popped entry is never stale and no
//                  filtering is needed.
//
//   lazy deletion  On an improved distance, push a new entry. The queue may
//                  hold several entries per node, so a popped entry must be
//                  checked against the current best distance and discarded if
//                  superseded.
//
// `if constexpr` means the unused branch is not merely skipped but absent from
// the generated code, so neither strategy pays for the other's existence.
//
// -----------------------------------------------------------------------------
// The settled check, and why it is not redundant
// -----------------------------------------------------------------------------
//
// The stale filter tests both `settled[u]` and `key > dist[u]`. The second
// alone is insufficient: a node can be pushed twice with the *same* key (two
// equally short paths discovered separately), in which case the second entry
// has key == dist[u] and would pass a key-only filter, causing the node to be
// settled and relaxed twice. That inflates relaxation counts -- a reported
// metric -- without changing the distances, so it would corrupt the
// measurement while leaving every correctness test green.
// =============================================================================
#ifndef LPQ_ALGORITHMS_DIJKSTRA_HPP
#define LPQ_ALGORITHMS_DIJKSTRA_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "lpq/algorithms/search_observer.hpp"
#include "lpq/algorithms/shortest_path_result.hpp"
#include "lpq/common/clock.hpp"
#include "lpq/common/numeric.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

/// Options for a Dijkstra run.
struct DijkstraOptions {
    /// Stop once this node is settled; kInvalidNode runs a full SSSP search.
    NodeId target = kInvalidNode;

    /// Include the timed region's duration in the result.
    ///
    /// Reading the clock twice per query is negligible, but the flag exists so
    /// that correctness sweeps -- which run hundreds of thousands of tiny
    /// queries -- are not dominated by clock reads.
    bool measure_time = true;
};

/// Runs Dijkstra from `source` using `queue`.
///
/// `queue` is passed by reference and cleared on entry rather than constructed
/// here, so the benchmark runner can allocate it once per graph and reuse it
/// across queries. Allocating a position map per query would make the
/// measurement largely an allocator benchmark (PRD.md section 24).
///
/// `observer` is the trace seam (search_observer.hpp). With the default
/// NullSearchObserver every hook is compiled out, so the timed search is
/// unchanged by its existence.
template <WeightType W, PriorityQueueLike Queue, typename Observer = NullSearchObserver>
[[nodiscard]] ShortestPathResult<W> dijkstra(const CsrGraph<W>& g, NodeId source, Queue& queue,
                                             const DijkstraOptions& opts = {},
                                             Observer* observer = nullptr) {
    using DistanceT = DistanceType<W>;
    static_assert(std::is_same_v<typename Queue::KeyType, DistanceT>,
                  "queue key type must match the graph's distance type");

    (void)observer;  // unreferenced when Observer is the null observer
    ShortestPathResult<W> r;
    const std::size_t     n = g.num_nodes();
    r.initialise(n, source);
    r.target = opts.target;

    if (n == 0 || source >= n) {
        r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                      : SearchTermination::kTargetUnreachable;
        return r;
    }

    queue.clear();
    // Queues are reused across queries, so the comparison counter must be
    // zeroed per query. Without this the count would accumulate over a whole
    // run and every per-query figure would be wrong.
    queue.reset_comparison_count();

    const TimePoint start = opts.measure_time ? now() : TimePoint{};

    queue.push(source, DistanceT{});
    count_event(r.counters.pq_pushes);
    if constexpr (kObserved<Observer>) {
        observer->on_source_pushed(source, 0.0);
    }

    while (!queue.empty()) {
        if constexpr (kInstrumentationEnabled) {
            r.counters.peak_queue_size = std::max(r.counters.peak_queue_size,
                                                  static_cast<std::uint64_t>(queue.size()));
        }

        const QueueEntry<DistanceT> entry = queue.pop();
        count_event(r.counters.pq_pops);
        const NodeId    u = entry.node;
        const DistanceT d = entry.key;
        if constexpr (kObserved<Observer>) {
            observer->on_pop(u, static_cast<double>(d), queue.size());
        }

        if constexpr (!Queue::kSupportsDecreaseKey) {
            // Lazy deletion: discard superseded entries. See the header comment
            // for why the settled check is needed alongside the key check.
            if (r.settled[u] || d > r.dist[u]) {
                count_event(r.counters.stale_pops);
                if constexpr (kObserved<Observer>) {
                    observer->on_stale_pop(u, static_cast<double>(d));
                }
                continue;
            }
        }

        r.settled[u] = true;
        count_event(r.counters.settled_nodes);
        if constexpr (kObserved<Observer>) {
            observer->on_settle(u, static_cast<double>(r.dist[u]));
        }

        if (opts.target != kInvalidNode && u == opts.target) {
            r.termination = SearchTermination::kTargetReached;
            if constexpr (kObserved<Observer>) {
                observer->on_target_reached(u, static_cast<double>(r.dist[u]));
            }
            if (opts.measure_time) {
                r.runtime_ns = elapsed_ns_since(start);
            }
            if constexpr (kInstrumentationEnabled) {
                r.counters.pq_comparisons = queue.comparison_count();
            }
            return r;
        }

        for (const Arc<W>& a : g.out_arcs(u)) {
            count_event(r.counters.relaxations);
            const DistanceT candidate = saturating_add<W>(d, a.weight);
            if constexpr (kObserved<Observer>) {
                observer->on_edge_examined(u, a.target, static_cast<double>(a.weight),
                                           static_cast<double>(candidate));
            }
            if (candidate < r.dist[a.target]) {
                if constexpr (kObserved<Observer>) {
                    const double old = r.dist[a.target] == kUnreachable<W>
                                           ? -1.0
                                           : static_cast<double>(r.dist[a.target]);
                    observer->on_edge_relaxed(u, a.target, old, static_cast<double>(candidate),
                                              static_cast<double>(candidate),
                                              Queue::kSupportsDecreaseKey
                                                  ? QueueUpdate::kDecreaseKey
                                                  : QueueUpdate::kPush);
                }
                r.dist[a.target]   = candidate;
                r.parent[a.target] = u;
                count_event(r.counters.successful_relaxations);

                if constexpr (Queue::kSupportsDecreaseKey) {
                    // Inserts when absent, lowers the key when present.
                    queue.decrease_key(a.target, candidate);
                    count_event(r.counters.pq_decrease_keys);
                } else {
                    queue.push(a.target, candidate);
                    count_event(r.counters.pq_pushes);
                }
            }
        }
    }

    r.termination = (opts.target == kInvalidNode) ? SearchTermination::kExhausted
                                                  : SearchTermination::kTargetUnreachable;
    if (opts.measure_time) {
        r.runtime_ns = elapsed_ns_since(start);
    }
    if constexpr (kInstrumentationEnabled) {
        r.counters.pq_comparisons = queue.comparison_count();
    }
    return r;
}

/// Convenience overload that constructs and sizes the queue itself.
///
/// For tests and one-off use. Benchmarks must use the reference-taking overload
/// so queue allocation stays outside the timed region.
template <WeightType W, PriorityQueueLike Queue>
[[nodiscard]] ShortestPathResult<W> dijkstra(const CsrGraph<W>& g, NodeId source,
                                             const DijkstraOptions& opts = {}) {
    Queue queue;
    queue.reserve_nodes(g.num_nodes());
    return dijkstra<W, Queue>(g, source, queue, opts);
}

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_DIJKSTRA_HPP
