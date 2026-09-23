// =============================================================================
// Array-backed binary heap with decrease-key (ESD.md section 7).
//
// This is the reference baseline for every speedup claim in the project
// (PRD.md section 19), so it is written to be genuinely fast rather than merely
// correct. An artificially slow baseline would manufacture a speedup for LPQ
// that says nothing about the research hypothesis.
//
// Performance choices, and why:
//
//   * Position map. `pos_[node]` gives the node's index in the heap array, so
//     decrease-key is O(log n) instead of an O(n) search. The map costs one
//     uint32 per graph node, allocated once per graph rather than per query.
//
//   * Sift-down by "hole" movement. The textbook sift-down swaps a pair at
//     every level, writing the moving element log n times. This implementation
//     carries the element in a register, moves only the displaced children into
//     the hole, and writes the carried element once at its final position --
//     roughly halving the memory traffic of the hottest loop in the queue.
//
//   * A 4-ary variant is provided as `FourAryHeap`. Wider fan-out means a
//     shallower tree (log4 vs log2) and fewer cache lines touched on the
//     sift-down path, at the cost of more comparisons per level. Which wins is
//     an empirical question about the workload, so both exist and the benchmark
//     decides rather than this comment.
//
//   * No virtual dispatch anywhere; Dijkstra binds to this type statically.
// =============================================================================
#ifndef LPQ_PQ_BINARY_HEAP_HPP
#define LPQ_PQ_BINARY_HEAP_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/metrics/phase_profiler.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

/// D-ary min-heap over node ids with O(log n) decrease-key.
///
/// `Arity` is the branching factor: 2 for a classical binary heap, 4 for the
/// cache-friendlier variant. Both satisfy `DecreaseKeyQueue`.
template <typename Key, std::size_t Arity = 2>
class DAryHeap {
    static_assert(Arity >= 2, "heap arity must be at least 2");

public:
    using KeyType = Key;

    /// This queue holds one entry per node and supports decrease-key, so
    /// Dijkstra will never push a duplicate entry for it.
    static constexpr bool        kSupportsDecreaseKey = true;
    static constexpr std::size_t kArity               = Arity;

    /// Sentinel for "this node is not in the heap".
    static constexpr std::uint32_t kNotInHeap = std::numeric_limits<std::uint32_t>::max();

    DAryHeap() = default;

    explicit DAryHeap(std::size_t node_count) { reserve_nodes(node_count); }

    /// Sizes the position map for a graph with `node_count` nodes.
    ///
    /// Called once per graph. Growing the map mid-search would be a
    /// reallocation inside a timed region, which is exactly what PRD.md
    /// section 24 forbids.
    void reserve_nodes(std::size_t node_count) {
        if (node_count > kMaxNodeCount) {
            throw InvalidConfigError("node count exceeds the NodeId range");
        }
        pos_.assign(node_count, kNotInHeap);
        heap_.reserve(node_count);
    }

    // --- Observers ------------------------------------------------------------

    [[nodiscard]] bool        empty() const noexcept { return heap_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }

    [[nodiscard]] QueueEntry<Key> top() const noexcept { return heap_.front(); }

    [[nodiscard]] bool contains(NodeId n) const noexcept {
        return n < pos_.size() && pos_[n] != kNotInHeap;
    }

    /// The node's current key. Only meaningful when `contains(n)`.
    [[nodiscard]] Key key_of(NodeId n) const noexcept { return heap_[pos_[n]].key; }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return cmp_.value(); }
    void                        reset_comparison_count() noexcept { cmp_.reset(); }

    // --- Mutators -------------------------------------------------------------

    /// Empties the heap, keeping the position map's allocation.
    ///
    /// Only the entries actually present are reset, so clearing between queries
    /// costs O(heap size) rather than O(node count). On a large graph where a
    /// target-mode query settles a small neighbourhood, that is the difference
    /// between a negligible reset and one that dominates the query.
    void clear() noexcept {
        for (const auto& e : heap_) {
            pos_[e.node] = kNotInHeap;
        }
        heap_.clear();
    }

    /// Inserts `n` with key `k`.
    ///
    /// If `n` is already present this delegates to decrease-key semantics: a
    /// lower key is applied, a higher one ignored. Dijkstra never does this --
    /// it calls decrease_key explicitly -- but making push total rather than
    /// partial removes a whole class of misuse from the LPQ paths that will
    /// later reinsert corrected candidates.
    void push(NodeId n, Key k) {
        if (contains(n)) {
            decrease_key(n, k);
            return;
        }
        // After the delegation above, so a push that becomes a decrease-key is
        // timed once rather than counted into this phase twice.
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
        heap_.push_back(QueueEntry<Key>{n, k});
        const auto idx = static_cast<std::uint32_t>(heap_.size() - 1U);
        pos_[n]        = idx;
        sift_up(idx);
    }

    /// Removes and returns the minimum entry. Undefined when empty.
    QueueEntry<Key> pop() {
        LPQ_PROFILE_SCOPE(QueuePhase::kPop);
        const QueueEntry<Key> best = heap_.front();
        pos_[best.node]            = kNotInHeap;

        // Move the last element into the root and sift it down. Popping the
        // last element must not sift, or it would read the just-removed slot.
        if (heap_.size() == 1U) {
            heap_.pop_back();
            return best;
        }
        heap_.front() = heap_.back();
        heap_.pop_back();
        pos_[heap_.front().node] = 0U;
        sift_down(0U);
        return best;
    }

    /// Lowers `n`'s key to `k`, inserting it if absent.
    ///
    /// A key that is not lower is ignored rather than rejected. Dijkstra
    /// already guards with `candidate < dist[v]`, but tolerating the no-op here
    /// keeps the queue safe for the LPQ correction paths, which may re-offer a
    /// key they have not proven to be an improvement.
    void decrease_key(NodeId n, Key k) {
        if (!contains(n)) {
            push(n, k);
            return;
        }
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
        const std::uint32_t idx = pos_[n];
        cmp_.count();
        if (!KeyLess<Key>{}(k, heap_[idx].key)) {
            return;
        }
        heap_[idx].key = k;
        sift_up(idx);
    }

private:
    // --- Heap mechanics -------------------------------------------------------

    [[nodiscard]] static constexpr std::uint32_t parent_of(std::uint32_t i) noexcept {
        return static_cast<std::uint32_t>((i - 1U) / Arity);
    }

    [[nodiscard]] static constexpr std::uint32_t first_child_of(std::uint32_t i) noexcept {
        return static_cast<std::uint32_t>(i * Arity + 1U);
    }

    /// Moves the element at `idx` toward the root until the heap order holds.
    ///
    /// Hole-based: the element is carried in `moving` and written once, rather
    /// than swapped at each level.
    void sift_up(std::uint32_t idx) noexcept {
        const QueueEntry<Key> moving = heap_[idx];
        while (idx > 0U) {
            const std::uint32_t parent = parent_of(idx);
            cmp_.count();
            if (!KeyLess<Key>{}(moving.key, heap_[parent].key)) {
                break;
            }
            heap_[idx]             = heap_[parent];
            pos_[heap_[idx].node]  = idx;
            idx                    = parent;
        }
        heap_[idx]      = moving;
        pos_[moving.node] = idx;
    }

    /// Moves the element at `idx` toward the leaves until the heap order holds.
    void sift_down(std::uint32_t idx) noexcept {
        const QueueEntry<Key> moving = heap_[idx];
        const auto            n      = static_cast<std::uint32_t>(heap_.size());

        for (;;) {
            const std::uint32_t first = first_child_of(idx);
            if (first >= n) {
                break;  // Leaf reached.
            }

            // Pick the smallest child. The last parent may have fewer than
            // `Arity` children, so the range is clamped.
            const std::uint32_t last     = std::min<std::uint32_t>(first + Arity, n);
            std::uint32_t       smallest = first;
            for (std::uint32_t c = first + 1U; c < last; ++c) {
                cmp_.count();
                if (KeyLess<Key>{}(heap_[c].key, heap_[smallest].key)) {
                    smallest = c;
                }
            }

            cmp_.count();
            if (!KeyLess<Key>{}(heap_[smallest].key, moving.key)) {
                break;  // Heap order restored.
            }
            heap_[idx]            = heap_[smallest];
            pos_[heap_[idx].node] = idx;
            idx                   = smallest;
        }
        heap_[idx]        = moving;
        pos_[moving.node] = idx;
    }

    std::vector<QueueEntry<Key>> heap_;
    /// pos_[node] is the node's index in heap_, or kNotInHeap.
    std::vector<std::uint32_t>   pos_;
    ComparisonCounter            cmp_;
};

/// Classical binary heap.
template <typename Key>
using BinaryHeap = DAryHeap<Key, 2>;

/// 4-ary heap: shallower tree, more comparisons per level. Benchmarked against
/// the binary heap rather than assumed better.
template <typename Key>
using FourAryHeap = DAryHeap<Key, 4>;

}  // namespace lpq

#endif  // LPQ_PQ_BINARY_HEAP_HPP
