// =============================================================================
// Pairing heap with decrease-key (ESD.md section 7).
//
// A self-adjusting heap-ordered multiway tree. Amortised O(1) insert, merge and
// decrease-key; O(log n) delete-min. Its practical appeal is that decrease-key
// is a pointer cut plus a single merge, with none of the Fibonacci heap's
// bookkeeping -- in practice it usually beats the Fibonacci heap despite worse
// theoretical bounds for decrease-key, which is a comparison this project
// measures rather than assumes.
//
// IMPLEMENTATION NOTES
//
//   * Node pool, not individual allocation. Nodes live in one contiguous
//     std::vector indexed by NodeId, and links are 32-bit indices rather than
//     pointers. Three reasons: allocating a node per insert would put malloc in
//     Dijkstra's hot path and make the measurement mostly an allocator
//     benchmark; indices halve link size against 64-bit pointers, so more of
//     the tree fits per cache line; and finding a graph node's heap node for
//     decrease-key becomes an array index rather than a map lookup.
//
//     Because the pool is indexed by NodeId, each graph node occupies at most
//     one heap node -- which is exactly the invariant a decrease-key queue
//     wants anyway.
//
//   * Child list as left-child/right-sibling. A multiway tree stored with two
//     links per node instead of a variable-length child array, so no node
//     owns a heap-allocated vector.
//
//   * Two-pass merge on delete-min. The standard pairing pass: merge siblings
//     left to right in pairs, then fold the results right to left. The naive
//     single left-to-right fold degrades to a linked list and loses the
//     amortised bound, so the two-pass structure is load-bearing, not stylistic.
//
//   * Iterative, not recursive. The two-pass merge is written with an explicit
//     stack. A recursive version recurses to the sibling-list length, which on
//     an adversarial insertion order is O(n) deep and overflows the stack on
//     large graphs -- a crash that would appear only on the biggest benchmarks.
// =============================================================================
#ifndef LPQ_PQ_PAIRING_HEAP_HPP
#define LPQ_PQ_PAIRING_HEAP_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

template <typename Key>
class PairingHeap {
public:
    using KeyType = Key;

    static constexpr bool kSupportsDecreaseKey = true;

    /// Null link sentinel.
    static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

    PairingHeap() = default;
    explicit PairingHeap(std::size_t node_count) { reserve_nodes(node_count); }

    void reserve_nodes(std::size_t node_count) {
        if (node_count > kMaxNodeCount) {
            throw InvalidConfigError("node count exceeds the NodeId range");
        }
        nodes_.assign(node_count, Node{});
        root_ = kNil;
        size_ = 0;
    }

    // --- Observers ------------------------------------------------------------

    [[nodiscard]] bool        empty() const noexcept { return root_ == kNil; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] QueueEntry<Key> top() const noexcept {
        return QueueEntry<Key>{static_cast<NodeId>(root_), nodes_[root_].key};
    }

    [[nodiscard]] bool contains(NodeId n) const noexcept {
        return n < nodes_.size() && nodes_[n].in_heap;
    }

    [[nodiscard]] Key key_of(NodeId n) const noexcept { return nodes_[n].key; }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return cmp_.value(); }
    void                        reset_comparison_count() noexcept { cmp_.reset(); }

    // --- Mutators -------------------------------------------------------------

    void clear() noexcept {
        // Only touch nodes that are actually in the heap, so clearing between
        // queries is proportional to the heap size rather than the graph size.
        if (root_ != kNil) {
            clear_stack_.clear();
            clear_stack_.push_back(root_);
            while (!clear_stack_.empty()) {
                const std::uint32_t v = clear_stack_.back();
                clear_stack_.pop_back();
                Node& node = nodes_[v];
                if (node.child != kNil) {
                    clear_stack_.push_back(node.child);
                }
                if (node.sibling != kNil) {
                    clear_stack_.push_back(node.sibling);
                }
                node = Node{};
            }
        }
        root_ = kNil;
        size_ = 0;
    }

    void push(NodeId n, Key k) {
        if (contains(n)) {
            decrease_key(n, k);
            return;
        }
        Node& node   = nodes_[n];
        node.key     = k;
        node.child   = kNil;
        node.sibling = kNil;
        node.prev    = kNil;
        node.in_heap = true;
        ++size_;
        root_ = (root_ == kNil) ? n : merge(root_, n);
    }

    QueueEntry<Key> pop() {
        const auto      old_root = root_;
        QueueEntry<Key> best{static_cast<NodeId>(old_root), nodes_[old_root].key};

        const std::uint32_t first_child = nodes_[old_root].child;
        nodes_[old_root]                = Node{};  // clears in_heap
        --size_;

        root_ = two_pass_merge(first_child);
        if (root_ != kNil) {
            nodes_[root_].prev    = kNil;
            nodes_[root_].sibling = kNil;
        }
        return best;
    }

    /// Lowers `n`'s key to `k`.
    ///
    /// Cuts the subtree rooted at `n` from its parent and merges it back at the
    /// root. Cutting is necessary because lowering a key can violate heap order
    /// against the parent but never against `n`'s own children, so the subtree
    /// stays valid and only its attachment point must change.
    void decrease_key(NodeId n, Key k) {
        if (!contains(n)) {
            push(n, k);
            return;
        }
        Node& node = nodes_[n];
        cmp_.count();
        if (!KeyLess<Key>{}(k, node.key)) {
            return;  // Not an improvement.
        }
        node.key = k;

        if (n == root_) {
            return;  // Already at the root; heap order cannot be violated.
        }
        detach(n);
        root_ = merge(root_, n);
    }

private:
    struct Node {
        Key           key{};
        std::uint32_t child   = kNil;
        std::uint32_t sibling = kNil;
        /// Parent for a first child, left sibling otherwise. One link serves
        /// both roles, which is what makes an O(1) detach possible without
        /// storing a separate parent pointer.
        std::uint32_t prev    = kNil;
        bool          in_heap = false;
    };

    /// Links two heap-ordered trees, returning the new root.
    [[nodiscard]] std::uint32_t merge(std::uint32_t a, std::uint32_t b) noexcept {
        if (a == kNil) {
            return b;
        }
        if (b == kNil) {
            return a;
        }
        cmp_.count();
        if (KeyLess<Key>{}(nodes_[b].key, nodes_[a].key)) {
            std::swap(a, b);
        }
        // `b` becomes the first child of `a`.
        nodes_[b].sibling = nodes_[a].child;
        if (nodes_[b].sibling != kNil) {
            nodes_[nodes_[b].sibling].prev = b;
        }
        nodes_[b].prev  = a;
        nodes_[a].child = b;
        return a;
    }

    /// Removes `n` from its parent's child list. O(1).
    void detach(std::uint32_t n) noexcept {
        Node&               node = nodes_[n];
        const std::uint32_t p    = node.prev;
        if (p == kNil) {
            return;
        }
        if (nodes_[p].child == n) {
            nodes_[p].child = node.sibling;  // n was the first child
        } else {
            nodes_[p].sibling = node.sibling;  // n was a sibling
        }
        if (node.sibling != kNil) {
            nodes_[node.sibling].prev = p;
        }
        node.sibling = kNil;
        node.prev    = kNil;
    }

    /// Two-pass merge of a sibling list, returning the resulting root.
    ///
    /// Pass one pairs adjacent siblings left to right; pass two folds those
    /// results right to left. Written iteratively: recursion here would be as
    /// deep as the sibling list, which is O(n) in the worst case.
    [[nodiscard]] std::uint32_t two_pass_merge(std::uint32_t first) noexcept {
        if (first == kNil) {
            return kNil;
        }
        pairs_.clear();

        // --- Pass one: merge in pairs ----------------------------------------
        std::uint32_t current = first;
        while (current != kNil) {
            const std::uint32_t a    = current;
            const std::uint32_t b    = nodes_[a].sibling;
            std::uint32_t       next = kNil;

            nodes_[a].sibling = kNil;
            nodes_[a].prev    = kNil;

            if (b == kNil) {
                pairs_.push_back(a);
                break;
            }
            next              = nodes_[b].sibling;
            nodes_[b].sibling = kNil;
            nodes_[b].prev    = kNil;
            pairs_.push_back(merge(a, b));
            current = next;
        }

        // --- Pass two: fold right to left ------------------------------------
        std::uint32_t result = pairs_.back();
        for (std::size_t i = pairs_.size() - 1U; i > 0U; --i) {
            result = merge(pairs_[i - 1U], result);
        }
        return result;
    }

    std::vector<Node>          nodes_;
    std::uint32_t              root_ = kNil;
    std::size_t                size_ = 0;
    /// Scratch buffers reused across calls so the hot path never allocates.
    std::vector<std::uint32_t> pairs_;
    std::vector<std::uint32_t> clear_stack_;
    ComparisonCounter          cmp_;
};

}  // namespace lpq

#endif  // LPQ_PQ_PAIRING_HEAP_HPP
