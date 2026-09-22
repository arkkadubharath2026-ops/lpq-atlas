// =============================================================================
// Monotone radix heap (ESD.md section 7).
//
// ESD.md section 7: "Support only compatible monotonic integer-key conditions.
// The benchmark runner must automatically mark incompatible configurations as
// unsupported rather than producing invalid results."
//
// PRECONDITIONS -- and why they are enforced rather than assumed
//
//   1. Integer keys. The bucket index is derived from the position of the
//      highest bit in which a key differs from the last extracted minimum,
//      which is a statement about the integer bit pattern. Floating-point keys
//      have no such structure, so `RadixHeapCompatible` rejects them at compile
//      time and the runner reports the configuration as unsupported.
//
//   2. Monotone extraction: keys pushed must never be below the last key
//      extracted. Dijkstra with non-negative weights satisfies this, because a
//      newly discovered distance is the settled distance plus a non-negative
//      weight. A* satisfies it only with a *consistent* heuristic; with a
//      merely admissible one, keys can decrease and the structure breaks. The
//      A* path therefore checks consistency before selecting this queue.
//
// A violation is a silent wrong answer rather than a crash: the key lands in a
// bucket that has already been emptied and is simply never returned. Debug
// builds therefore assert monotonicity, and the release build documents it.
//
// WHY IT CAN BEAT A COMPARISON HEAP
//
// Insert is O(1): compute a bucket index with a bit trick and append. There is
// no sift, and no comparison at all. Delete-min is amortised O(log C) for a
// maximum key range C, and crucially the redistribution cost is charged to
// elements that only ever move to lower-numbered buckets. On road networks,
// where weights are small integers, this is often the fastest queue available,
// which is exactly why it belongs in the baseline matrix: LPQ must be compared
// against the strongest classical queue for the workload, not just the most
// familiar one (PRD.md section 28, "do not compare against weak baselines").
// =============================================================================
#ifndef LPQ_PQ_RADIX_HEAP_HPP
#define LPQ_PQ_RADIX_HEAP_HPP

#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

/// Keys a radix heap can order. Unsigned integers only; see preconditions above.
template <typename Key>
concept RadixHeapCompatible = std::unsigned_integral<Key>;

/// Monotone radix heap over unsigned integer keys.
///
/// Uses lazy deletion rather than decrease-key: an improved key is pushed as a
/// new entry and the superseded one is recognised when popped. A radix heap
/// cannot support decrease-key efficiently, because lowering a key would have
/// to locate and move the entry between buckets, and entries are stored in flat
/// per-bucket vectors with no position index. Lazy deletion is the standard and
/// cheaper resolution; Dijkstra handles it through the same code path it uses
/// for every other queue.
template <RadixHeapCompatible Key>
class RadixHeap {
public:
    using KeyType = Key;

    /// Lazy deletion, not decrease-key. Dijkstra branches on this at compile
    /// time and will filter stale pops for this queue.
    static constexpr bool kSupportsDecreaseKey = false;

    /// One bucket per bit position, plus one for keys equal to the current
    /// minimum and one guard. For a 64-bit key that is 66 buckets -- small,
    /// fixed, and allocated inline.
    static constexpr std::size_t kNumBuckets = std::numeric_limits<Key>::digits + 2U;

    RadixHeap() = default;
    explicit RadixHeap(std::size_t node_count) { reserve_nodes(node_count); }

    /// The radix heap needs no per-node storage, so this only reserves capacity
    /// in bucket 0, where Dijkstra's initial pushes land.
    void reserve_nodes(std::size_t node_count) {
        buckets_[0].reserve(node_count > 1024U ? 1024U : node_count);
    }

    // --- Observers ------------------------------------------------------------

    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// The minimum entry. Redistributes buckets if needed, so it is not const.
    [[nodiscard]] QueueEntry<Key> top() const noexcept {
        // top() is logically const but may need to pull the minimum forward.
        // The mutation is an internal reorganisation that leaves the multiset
        // of entries unchanged, so const_cast is honest here rather than a
        // loophole: no observable state changes.
        auto* self = const_cast<RadixHeap*>(this);
        self->ensure_min_bucket();
        return self->buckets_[0].back();
    }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return cmp_.value(); }
    void                        reset_comparison_count() noexcept { cmp_.reset(); }

    /// Last extracted key. Pushing below this violates monotonicity.
    [[nodiscard]] Key last_extracted() const noexcept { return last_; }

    // --- Mutators -------------------------------------------------------------

    void clear() noexcept {
        for (auto& b : buckets_) {
            b.clear();
        }
        size_ = 0;
        last_ = Key{};
    }

    void push(NodeId n, Key k) {
        // Monotonicity is the structure's correctness precondition. Violating
        // it puts the entry in an already-drained bucket, where it is lost
        // silently -- the worst possible failure mode for a shortest-path
        // result, so it is asserted rather than trusted.
        assert(k >= last_ && "radix heap requires monotone keys");
        buckets_[bucket_for(k)].push_back(QueueEntry<Key>{n, k});
        ++size_;
    }

    QueueEntry<Key> pop() {
        ensure_min_bucket();
        const QueueEntry<Key> best = buckets_[0].back();
        buckets_[0].pop_back();
        --size_;
        return best;
    }

private:
    /// Bucket index for key `k` relative to the last extracted key.
    ///
    /// The index is the position of the highest set bit of `k XOR last_`,
    /// plus one; equal keys go to bucket 0. Keys that agree with `last_` in
    /// more high bits are closer to it and land in lower buckets, which is what
    /// makes the redistribution monotone: an element can only ever move to a
    /// lower-numbered bucket, so each element is redistributed O(log C) times
    /// over its whole lifetime.
    [[nodiscard]] std::size_t bucket_for(Key k) const noexcept {
        const Key x = static_cast<Key>(k ^ last_);
        if (x == 0) {
            return 0;
        }
        // bit_width gives the 1-based position of the highest set bit.
        return static_cast<std::size_t>(std::bit_width(x));
    }

    /// Guarantees that bucket 0 holds the minimum entry.
    ///
    /// When bucket 0 is empty, finds the lowest non-empty bucket, extracts its
    /// minimum as the new `last_`, and redistributes that bucket's contents.
    /// Every element moves strictly downward, which is where the amortised
    /// bound comes from.
    void ensure_min_bucket() noexcept {
        if (!buckets_[0].empty()) {
            return;
        }
        std::size_t b = 1;
        while (b < kNumBuckets && buckets_[b].empty()) {
            ++b;
        }
        if (b == kNumBuckets) {
            return;  // Heap is empty; pop() on an empty heap is undefined.
        }

        // Find the minimum key in that bucket: it becomes the new reference.
        Key new_min = buckets_[b].front().key;
        for (const auto& e : buckets_[b]) {
            cmp_.count();
            if (e.key < new_min) {
                new_min = e.key;
            }
        }
        last_ = new_min;

        // Redistribute. Moving out of a bucket we are draining means indices
        // are recomputed against the new `last_`, so entries equal to it land
        // in bucket 0 and the rest strictly below b.
        auto& source = buckets_[b];
        for (const auto& e : source) {
            buckets_[bucket_for(e.key)].push_back(e);
        }
        source.clear();
    }

    std::array<std::vector<QueueEntry<Key>>, kNumBuckets> buckets_{};
    std::size_t                                           size_ = 0;
    /// Last extracted key; the reference point for bucket indexing.
    Key                                                   last_ = Key{};
    ComparisonCounter                                     cmp_;
};

// -----------------------------------------------------------------------------
// Compatibility reporting
// -----------------------------------------------------------------------------

/// Whether a radix heap may be used for the given weight type.
///
/// The benchmark runner calls this and records the configuration as
/// "unsupported" instead of running it, so a missing radix-heap row in a
/// results table is explained rather than merely absent (ESD.md section 7).
template <WeightType W>
[[nodiscard]] constexpr bool radix_heap_supports() noexcept {
    return std::unsigned_integral<DistanceType<W>>;
}

/// Human-readable reason a radix heap is unavailable, for result metadata.
template <WeightType W>
[[nodiscard]] constexpr const char* radix_heap_unsupported_reason() noexcept {
    if constexpr (radix_heap_supports<W>()) {
        return "";
    } else {
        return "radix heap requires unsigned integer keys; this graph uses "
               "floating-point weights, whose bit patterns do not yield a "
               "monotone bucket index";
    }
}

}  // namespace lpq

#endif  // LPQ_PQ_RADIX_HEAP_HPP
