// =============================================================================
// The priority-queue interface (ESD.md section 6).
//
// ESD.md sketches the interface as an abstract base class with virtual methods,
// then immediately notes: "For performance-critical production implementations,
// avoid virtual calls in the hot path by using templates/concepts or
// compile-time dispatch. The interface is conceptual; the final implementation
// should optimize away abstraction overhead."
//
// This project takes that instruction literally. There is no abstract base
// class. The contract is expressed as a C++20 concept, and Dijkstra is a
// template parameterised on the queue type, so every call is statically bound
// and inlinable. That matters more here than in most code: the whole research
// question is whether LPQ's savings exceed its ML overhead, and a virtual call
// per extract-min would add a cost to the *baselines* that has nothing to do
// with the algorithms being compared. An unfair baseline would make LPQ look
// good for the wrong reason (PRD.md section 19, README section 7).
//
// -----------------------------------------------------------------------------
// Two key-update strategies
// -----------------------------------------------------------------------------
//
// ESD.md section 8 requires the choice between decrease-key and lazy deletion
// to be made consistently per queue and documented. Both are supported, and a
// queue declares which it provides via `kSupportsDecreaseKey`:
//
//   Decrease-key  The queue holds at most one entry per node and can lower an
//                 existing key in place. Needs a node->position map. Used by
//                 the binary, pairing and Fibonacci heaps.
//
//   Lazy deletion The queue may hold several entries per node; superseded ones
//                 are recognised and discarded when popped. No position map,
//                 cheaper pushes, more memory, more pops. Used by the radix
//                 heap and available as a variant for the binary heap.
//
// Dijkstra handles both from one code path, branching at compile time on
// `kSupportsDecreaseKey`, so the two strategies cost nothing to support.
// =============================================================================
#ifndef LPQ_PQ_PRIORITY_QUEUE_HPP
#define LPQ_PQ_PRIORITY_QUEUE_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "lpq/common/types.hpp"

namespace lpq {

/// What a queue pops: the node and the key it was stored under.
template <typename Key>
struct QueueEntry {
    NodeId node;
    Key    key;

    friend constexpr bool operator==(const QueueEntry&, const QueueEntry&) = default;
};

// -----------------------------------------------------------------------------
// Concepts
// -----------------------------------------------------------------------------

/// Operations every priority queue must provide.
///
/// `Key` is the priority type: DistanceType<W> for Dijkstra, and the same for
/// A* where the key is g + h rather than g.
template <typename Q>
concept PriorityQueueLike = requires(Q q, const Q cq, NodeId n, typename Q::KeyType k) {
    typename Q::KeyType;

    { q.push(n, k) }        -> std::same_as<void>;
    { q.pop() }             -> std::same_as<QueueEntry<typename Q::KeyType>>;
    { cq.top() }            -> std::same_as<QueueEntry<typename Q::KeyType>>;
    { cq.empty() }          -> std::same_as<bool>;
    { cq.size() }           -> std::same_as<std::size_t>;
    { q.clear() }           -> std::same_as<void>;

    /// Number of key comparisons performed. The machine-independent measure of
    /// queue work (PRD.md section 20), and comparable across implementations
    /// in a way wall-clock time is not.
    { cq.comparison_count() } -> std::convertible_to<std::uint64_t>;

    /// Zeroes the comparison counter. Queues are reused across queries, so a
    /// per-query count requires an explicit reset; without it the counter would
    /// accumulate across every query in a run.
    { q.reset_comparison_count() } -> std::same_as<void>;

    /// Compile-time declaration of the key-update strategy.
    { Q::kSupportsDecreaseKey } -> std::convertible_to<bool>;
};

/// A queue that can lower an existing node's key in place.
template <typename Q>
concept DecreaseKeyQueue = PriorityQueueLike<Q> && Q::kSupportsDecreaseKey &&
    requires(Q q, NodeId n, typename Q::KeyType k) {
        /// Lowers node `n`'s key to `k`, inserting it if absent.
        ///
        /// Callers must not raise a key: Dijkstra only ever improves a distance,
        /// and permitting increases would require sift-down logic whose cost
        /// the heaps are not paying for.
        { q.decrease_key(n, k) } -> std::same_as<void>;

        { q.contains(n) } -> std::same_as<bool>;
    };

/// A queue that relies on lazy deletion instead of decrease-key.
template <typename Q>
concept LazyDeletionQueue = PriorityQueueLike<Q> && (!Q::kSupportsDecreaseKey);

/// A queue that must be told the node count before use.
///
/// The decrease-key queues size their position map from it; the radix heap
/// sizes its buckets from the weight bound. Queues are reused across queries in
/// the benchmark runner, so allocation happens once per graph rather than once
/// per query -- otherwise the measurement would be dominated by allocation.
template <typename Q>
concept SizedQueue = PriorityQueueLike<Q> && requires(Q q, std::size_t n) {
    { q.reserve_nodes(n) } -> std::same_as<void>;
};

// -----------------------------------------------------------------------------
// Comparison counting
// -----------------------------------------------------------------------------

/// Counts key comparisons when instrumentation is enabled, and compiles to
/// nothing when it is not.
///
/// PRD.md section 20 requires comparison counts, but ESD.md section 19 requires
/// instrumentation to be compile-time configurable so that timed runs can
/// measure the true hot path. With LPQ_ENABLE_INSTRUMENTATION=0 this holds no
/// state and `count()` is a no-op, so the counter increment disappears from the
/// generated code entirely rather than merely being skipped at run time.
class ComparisonCounter {
public:
#if LPQ_ENABLE_INSTRUMENTATION
    void                        count(std::uint64_t n = 1) noexcept { count_ += n; }
    [[nodiscard]] std::uint64_t value() const noexcept { return count_; }
    void                        reset() noexcept { count_ = 0; }

private:
    std::uint64_t count_ = 0;
#else
    void count(std::uint64_t = 1) noexcept {}
    [[nodiscard]] static constexpr std::uint64_t value() noexcept { return 0; }
    void reset() noexcept {}
#endif
};

/// Ordering used by every queue in this project.
///
/// Plain `<` on the key, with no epsilon, even for double keys. An
/// epsilon-tolerant comparator is not transitive -- a ~ b and b ~ c does not
/// imply a ~ c -- so it is not a strict weak ordering and would corrupt heap
/// invariants in ways that surface as rare, data-dependent wrong answers.
/// Tolerance belongs in result *validation* (numeric.hpp), never in ordering.
///
/// Ties are broken by the queue's internal structure, which differs between
/// implementations. That is legitimate: any node with the minimum key is a
/// valid extraction for Dijkstra. Tests therefore assert that a popped key is
/// minimal, not that a particular node was popped.
template <typename Key>
struct KeyLess {
    [[nodiscard]] constexpr bool operator()(const Key& a, const Key& b) const noexcept {
        return a < b;
    }
};

}  // namespace lpq

#endif  // LPQ_PQ_PRIORITY_QUEUE_HPP
