// =============================================================================
// Fibonacci heap (ESD.md section 7).
//
// O(1) amortised insert and decrease-key, O(log n) amortised delete-min --
// the bounds that give Dijkstra its textbook O(m + n log n). ESD.md section 7
// asks for "a correct amortised implementation" and adds: "carefully benchmark
// actual constant factors."
//
// That caveat is the point of including it. The Fibonacci heap has the best
// asymptotic decrease-key of any queue here and, in practice, very often loses
// to a plain binary heap: every node carries five links plus a mark bit, the
// root list is pointer-chasing over scattered memory, and consolidation touches
// a degree table on every delete-min. Including it makes the baseline matrix
// honest (PRD.md section 19) and lets the paper report where theory and
// measurement diverge, rather than quietly omitting an awkward baseline.
//
// IMPLEMENTATION NOTES
//
//   * Pool-allocated, index-linked, exactly as PairingHeap and for the same
//     reasons: no malloc in the hot path, 32-bit links, and O(1) lookup from a
//     graph node to its heap node for decrease-key.
//
//   * Circular doubly-linked sibling lists. Circular lists make splice and
//     remove branch-free in the common case -- no null checks at list ends --
//     which is what keeps the constant factor from being even worse.
//
//   * Cascading cuts with mark bits. A node is marked when it loses its first
//     child; losing a second cuts it too, recursively. This is what bounds
//     subtree degeneration and preserves the amortised bound. Without marks the
//     structure still returns correct answers but the complexity argument
//     collapses, so the marks are load-bearing rather than decorative.
//
//   * The consolidation degree table is sized from a golden-ratio bound:
//     max degree <= log_phi(n). Sized once per graph, not per delete-min.
// =============================================================================
#ifndef LPQ_PQ_FIBONACCI_HEAP_HPP
#define LPQ_PQ_FIBONACCI_HEAP_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

template <typename Key>
class FibonacciHeap {
public:
    using KeyType = Key;

    static constexpr bool kSupportsDecreaseKey = true;

    static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

    FibonacciHeap() = default;
    explicit FibonacciHeap(std::size_t node_count) { reserve_nodes(node_count); }

    void reserve_nodes(std::size_t node_count) {
        if (node_count > kMaxNodeCount) {
            throw InvalidConfigError("node count exceeds the NodeId range");
        }
        nodes_.assign(node_count, Node{});
        min_  = kNil;
        size_ = 0;

        // Maximum degree of any node in a Fibonacci heap of n elements is
        // bounded by log_phi(n). Allocating the table once here keeps
        // consolidation allocation-free.
        const std::size_t max_degree =
            node_count < 2
                ? 2U
                : static_cast<std::size_t>(std::log(static_cast<double>(node_count)) /
                                           std::log(1.6180339887498948)) + 2U;
        degree_table_.assign(max_degree, kNil);
    }

    // --- Observers ------------------------------------------------------------

    [[nodiscard]] bool        empty() const noexcept { return min_ == kNil; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] QueueEntry<Key> top() const noexcept {
        return QueueEntry<Key>{static_cast<NodeId>(min_), nodes_[min_].key};
    }

    [[nodiscard]] bool contains(NodeId n) const noexcept {
        return n < nodes_.size() && nodes_[n].in_heap;
    }

    [[nodiscard]] Key key_of(NodeId n) const noexcept { return nodes_[n].key; }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return cmp_.value(); }
    void                        reset_comparison_count() noexcept { cmp_.reset(); }

    // --- Mutators -------------------------------------------------------------

    void clear() noexcept {
        if (min_ != kNil) {
            stack_.clear();
            stack_.push_back(min_);
            while (!stack_.empty()) {
                const std::uint32_t start = stack_.back();
                stack_.pop_back();
                // Walk this circular sibling list, resetting each node and
                // queueing its child list.
                std::uint32_t v = start;
                do {
                    const std::uint32_t next  = nodes_[v].right;
                    const std::uint32_t child = nodes_[v].child;
                    nodes_[v]                 = Node{};
                    if (child != kNil) {
                        stack_.push_back(child);
                    }
                    v = next;
                } while (v != start && v != kNil);
            }
        }
        min_  = kNil;
        size_ = 0;
        std::fill(degree_table_.begin(), degree_table_.end(), kNil);
    }

    void push(NodeId n, Key k) {
        if (contains(n)) {
            decrease_key(n, k);
            return;
        }
        Node& node   = nodes_[n];
        node.key     = k;
        node.parent  = kNil;
        node.child   = kNil;
        node.degree  = 0;
        node.marked  = false;
        node.in_heap = true;

        insert_into_root_list(n);
        ++size_;
    }

    QueueEntry<Key> pop() {
        const std::uint32_t z = min_;
        QueueEntry<Key>     best{static_cast<NodeId>(z), nodes_[z].key};

        // Promote z's children to the root list; they are heap-ordered among
        // themselves and only lose their parent.
        const std::uint32_t child = nodes_[z].child;
        if (child != kNil) {
            std::uint32_t v = child;
            do {
                const std::uint32_t next = nodes_[v].right;
                nodes_[v].parent         = kNil;
                nodes_[v].marked         = false;
                insert_into_root_list(v);
                v = next;
            } while (v != child);
        }

        remove_from_root_list(z);
        const std::uint32_t next_root = nodes_[z].right;
        nodes_[z]                     = Node{};  // clears in_heap
        --size_;

        if (z == next_root) {
            min_ = kNil;  // z was the only root and had no children
        } else {
            min_ = next_root;
            consolidate();
        }
        return best;
    }

    void decrease_key(NodeId n, Key k) {
        if (!contains(n)) {
            push(n, k);
            return;
        }
        Node& node = nodes_[n];
        cmp_.count();
        if (!KeyLess<Key>{}(k, node.key)) {
            return;
        }
        node.key = k;

        const std::uint32_t parent = node.parent;
        if (parent != kNil) {
            cmp_.count();
            if (KeyLess<Key>{}(node.key, nodes_[parent].key)) {
                cut(n, parent);
                cascading_cut(parent);
            }
        }
        cmp_.count();
        if (KeyLess<Key>{}(node.key, nodes_[min_].key)) {
            min_ = n;
        }
    }

private:
    struct Node {
        Key           key{};
        std::uint32_t parent = kNil;
        std::uint32_t child  = kNil;
        std::uint32_t left   = kNil;
        std::uint32_t right  = kNil;
        std::uint32_t degree = 0;
        /// True once this node has lost a child since becoming a child itself.
        bool          marked  = false;
        bool          in_heap = false;
    };

    /// Splices `n` into the root list next to `min_`, updating `min_`.
    void insert_into_root_list(std::uint32_t n) noexcept {
        if (min_ == kNil) {
            nodes_[n].left  = n;
            nodes_[n].right = n;
            min_            = n;
            return;
        }
        const std::uint32_t r = nodes_[min_].right;
        nodes_[n].left        = min_;
        nodes_[n].right       = r;
        nodes_[min_].right    = n;
        nodes_[r].left        = n;

        cmp_.count();
        if (KeyLess<Key>{}(nodes_[n].key, nodes_[min_].key)) {
            min_ = n;
        }
    }

    void remove_from_root_list(std::uint32_t n) noexcept {
        nodes_[nodes_[n].left].right = nodes_[n].right;
        nodes_[nodes_[n].right].left = nodes_[n].left;
    }

    /// Merges roots of equal degree until every degree is unique.
    void consolidate() noexcept {
        std::fill(degree_table_.begin(), degree_table_.end(), kNil);

        // Snapshot the root list first: linking mutates it while we iterate,
        // and walking a list that is being restructured is how consolidation
        // implementations most often go subtly wrong.
        roots_.clear();
        std::uint32_t v = min_;
        do {
            roots_.push_back(v);
            v = nodes_[v].right;
        } while (v != min_);

        for (std::uint32_t w : roots_) {
            std::uint32_t x = w;
            std::uint32_t d = nodes_[x].degree;
            while (d < degree_table_.size() && degree_table_[d] != kNil) {
                std::uint32_t y = degree_table_[d];
                if (y == x) {
                    break;
                }
                cmp_.count();
                if (KeyLess<Key>{}(nodes_[y].key, nodes_[x].key)) {
                    std::swap(x, y);
                }
                link(y, x);          // y becomes a child of x
                degree_table_[d] = kNil;
                ++d;
            }
            if (d < degree_table_.size()) {
                degree_table_[d] = x;
            }
        }

        // Rebuild min_ from the surviving roots.
        min_ = kNil;
        for (std::uint32_t r : degree_table_) {
            if (r == kNil) {
                continue;
            }
            if (min_ == kNil) {
                nodes_[r].left  = r;
                nodes_[r].right = r;
                min_            = r;
            } else {
                insert_into_root_list(r);
            }
        }
    }

    /// Makes `y` a child of `x`. Both must currently be roots.
    void link(std::uint32_t y, std::uint32_t x) noexcept {
        remove_from_root_list(y);
        nodes_[y].parent = x;
        nodes_[y].marked = false;

        const std::uint32_t c = nodes_[x].child;
        if (c == kNil) {
            nodes_[x].child = y;
            nodes_[y].left  = y;
            nodes_[y].right = y;
        } else {
            const std::uint32_t r = nodes_[c].right;
            nodes_[y].left        = c;
            nodes_[y].right       = r;
            nodes_[c].right       = y;
            nodes_[r].left        = y;
        }
        ++nodes_[x].degree;
    }

    /// Removes `n` from its parent's child list and moves it to the root list.
    void cut(std::uint32_t n, std::uint32_t parent) noexcept {
        if (nodes_[n].right == n) {
            nodes_[parent].child = kNil;  // n was the only child
        } else {
            nodes_[nodes_[n].left].right = nodes_[n].right;
            nodes_[nodes_[n].right].left = nodes_[n].left;
            if (nodes_[parent].child == n) {
                nodes_[parent].child = nodes_[n].right;
            }
        }
        --nodes_[parent].degree;

        nodes_[n].parent = kNil;
        nodes_[n].marked = false;
        insert_into_root_list(n);
    }

    /// Propagates cuts up the tree. A node losing its second child is itself
    /// cut, which is what keeps subtrees from degenerating.
    void cascading_cut(std::uint32_t n) noexcept {
        std::uint32_t current = n;
        for (;;) {
            const std::uint32_t parent = nodes_[current].parent;
            if (parent == kNil) {
                return;
            }
            if (!nodes_[current].marked) {
                nodes_[current].marked = true;
                return;
            }
            cut(current, parent);
            current = parent;
        }
    }

    std::vector<Node>          nodes_;
    std::uint32_t              min_  = kNil;
    std::size_t                size_ = 0;
    std::vector<std::uint32_t> degree_table_;
    /// Scratch buffers reused across calls so the hot path never allocates.
    std::vector<std::uint32_t> roots_;
    std::vector<std::uint32_t> stack_;
    ComparisonCounter          cmp_;
};

}  // namespace lpq

#endif  // LPQ_PQ_FIBONACCI_HEAP_HPP
