// =============================================================================
// Pooled bucket queue: a cache-resident monotone bucket queue.
//
// The vector-backed BucketSubstrate keeps one std::vector per bucket. That is
// fine for a wide bucket, but a narrow-bucket configuration needs thousands of
// buckets, and then the vector headers (24 bytes each) plus their separately
// allocated storage scatter across the cache: measured, the queue spent its time
// missing cache rather than comparing keys.
//
// This structure keeps the same idea -- a ring of buckets over a sliding key
// window, a hierarchical bitmap to jump to the next occupied one -- in a layout
// sized by what is actually queued rather than by the key range:
//
//   head_[ring]   one 32-bit index per bucket (width-1 ring of 65536 = 256 KiB)
//   pool_         entries {node, next} (8 bytes), recycled through a free list,
//                 so the working set is the live frontier (thousands of entries
//                 on a road network), not the number of pushes. Keys are stored
//                 in a parallel array only for width > 1: in a width-1 bucket
//                 the key IS the bucket, so storing it would double the entry
//                 for nothing.
//
// Extraction from a bucket of width 1 is a pointer swing: every entry in it has
// the same key, so there is nothing to compare. For wider buckets it scans the
// (short) list for the minimum, which is the classical bucket queue.
//
// Exactness: the bucket window only ever advances to the next occupied bucket,
// every key in an occupied bucket lies below every key in a later one, and keys
// beyond the ring wait in an overflow list that is re-filed as the window
// advances (the same invariant BucketSubstrate documents). Keys must be
// extracted in non-decreasing order, as in Dijkstra and A* with a consistent
// heuristic; a push below the window base is reported as an error.
// =============================================================================
#ifndef LPQ_LPQ_POOLED_BUCKET_QUEUE_HPP
#define LPQ_LPQ_POOLED_BUCKET_QUEUE_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/lpq/bucket_bitmap.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

template <WeightType W>
    requires(std::is_integral_v<W>)
class PooledBucketQueue {
public:
    using KeyType = DistanceType<W>;

    static constexpr bool kSupportsDecreaseKey = false;
    static constexpr std::size_t kMaxRing = std::size_t{1} << 22;

    PooledBucketQueue() : PooledBucketQueue(1, 1024) {}

    PooledBucketQueue(KeyType width, std::size_t ring_size) {
        if (width == 0) {
            throw InvalidConfigError("bucket width must be at least 1");
        }
        if (ring_size < 2 || ring_size > kMaxRing || !std::has_single_bit(ring_size)) {
            throw InvalidConfigError("bucket ring size must be a power of two in [2, " +
                                     std::to_string(kMaxRing) + "], got " +
                                     std::to_string(ring_size));
        }
        width_ = width;
        ring_  = ring_size;
        mask_  = ring_size - 1;
        head_.assign(ring_, kNil);
        nonempty_.reset(ring_);
    }

    void push(NodeId node, KeyType key) {
        if (key < base_) {
            throw InternalError(
                "pooled bucket queue received a key below the current base (" +
                std::to_string(static_cast<std::uint64_t>(key)) + " < " +
                std::to_string(static_cast<std::uint64_t>(base_)) +
                "); extracted keys must be non-decreasing");
        }
        ++size_;
        const std::size_t offset = static_cast<std::size_t>((key - base_) / width_);
        if (offset >= ring_) {
            overflow_.push_back(Spilled{node, key});
            if (key < overflow_min_) {
                overflow_min_ = key;
            }
            return;
        }
        link(node, key, (cursor_ + offset) & mask_);
    }

    [[nodiscard]] QueueEntry<KeyType> pop() {
        if (size_ == 0) {
            throw InternalError("pop() called on an empty pooled bucket queue");
        }
        advance();

        std::uint32_t* link_to = &head_[cursor_];
        std::uint32_t  idx     = *link_to;
        KeyType        key     = base_;  // width 1: the bucket is the key

        if (width_ != 1) {
            // Wider bucket: find the minimum. Strict < keeps the first of equal
            // keys; any of them is a valid minimum.
            std::uint32_t* best_link = link_to;
            std::uint32_t  best      = idx;
            for (std::uint32_t* l = link_to; *l != kNil; l = &pool_[*l].next) {
                ++comparisons_;
                if (keys_[*l] < keys_[best]) {
                    best      = *l;
                    best_link = l;
                }
            }
            link_to = best_link;
            idx     = best;
            key     = keys_[idx];
        }

        const Item taken = pool_[idx];
        *link_to         = taken.next;
        pool_[idx].next  = free_;
        free_            = idx;
        --size_;
        if (head_[cursor_] == kNil) {
            nonempty_.clear(cursor_);
        }
        return QueueEntry<KeyType>{taken.node, key};
    }

    [[nodiscard]] QueueEntry<KeyType> top() const {
        if (size_ == 0) {
            throw InternalError("top() called on an empty pooled bucket queue");
        }
        // Cold path (Dijkstra calls pop()): a copy keeps top() genuinely const.
        PooledBucketQueue copy(*this);
        const QueueEntry<KeyType> e = copy.pop();
        return e;
    }

    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    void clear() noexcept {
        for (std::size_t i = nonempty_.find_next(0); i != BucketBitmap::npos;
             i = nonempty_.find_next(i + 1)) {
            head_[i] = kNil;
        }
        nonempty_.clear_all();
        pool_.clear();
        keys_.clear();
        free_         = kNil;
        overflow_.clear();
        overflow_min_ = kNoKey;
        size_         = 0;
        cursor_       = 0;
        base_         = 0;
    }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return comparisons_; }
    void reset_comparison_count() noexcept { comparisons_ = 0; }
    void reserve_nodes(std::size_t) {}

    [[nodiscard]] KeyType     width() const noexcept { return width_; }
    [[nodiscard]] std::size_t ring_size() const noexcept { return ring_; }

private:
    static constexpr std::uint32_t kNil   = 0xFFFFFFFFU;
    static constexpr KeyType       kNoKey = std::numeric_limits<KeyType>::max();

    struct Item {
        NodeId        node;
        std::uint32_t next;
    };

    struct Spilled {  // an entry beyond the ring: carries its own key
        NodeId  node;
        KeyType key;
    };

    void link(NodeId node, KeyType key, std::size_t bucket) {
        std::uint32_t idx;
        if (free_ != kNil) {
            idx   = free_;
            free_ = pool_[idx].next;
        } else {
            idx = static_cast<std::uint32_t>(pool_.size());
            pool_.push_back(Item{});
            if (width_ != 1) {
                keys_.push_back(0);
            }
        }
        pool_[idx]    = Item{node, head_[bucket]};
        if (width_ != 1) {
            keys_[idx] = key;
        }
        head_[bucket] = idx;
        nonempty_.set(bucket);
    }

    /// Moves the cursor to the first occupied bucket, re-filing overflow keys the
    /// window has come to cover.
    void advance() {
        absorb();
        if (head_[cursor_] != kNil) {
            return;
        }
        for (;;) {
            std::size_t next = nonempty_.find_next(cursor_);
            if (next == BucketBitmap::npos) {
                next = nonempty_.find_next(0);
            }
            if (next == BucketBitmap::npos) {
                refile_all_overflow();
                continue;
            }
            base_ += static_cast<KeyType>((next - cursor_) & mask_) * width_;
            cursor_ = next;
            absorb();
            return;
        }
    }

    void absorb() {
        if (overflow_min_ == kNoKey) {
            return;
        }
        const KeyType window_end = base_ + static_cast<KeyType>(ring_) * width_;
        if (overflow_min_ >= window_end) {
            return;
        }
        std::vector<Spilled> pending;
        pending.swap(overflow_);
        overflow_min_ = kNoKey;
        for (const Spilled& e : pending) {
            if (e.key < window_end) {
                link(e.node, e.key,
                     (cursor_ + static_cast<std::size_t>((e.key - base_) / width_)) & mask_);
            } else {
                overflow_.push_back(e);
                if (e.key < overflow_min_) {
                    overflow_min_ = e.key;
                }
            }
        }
    }

    /// The ring is empty and everything is in overflow: jump the window there.
    void refile_all_overflow() {
        if (overflow_.empty()) {
            throw InternalError("pooled bucket queue is non-empty but ring and overflow are "
                                "empty; size counter and contents have diverged");
        }
        base_   = overflow_min_ - (overflow_min_ % width_);
        cursor_ = 0;
        std::vector<Spilled> pending;
        pending.swap(overflow_);
        overflow_min_ = kNoKey;
        for (const Spilled& e : pending) {
            const std::size_t offset = static_cast<std::size_t>((e.key - base_) / width_);
            if (offset >= ring_) {
                overflow_.push_back(e);
                if (e.key < overflow_min_) {
                    overflow_min_ = e.key;
                }
            } else {
                link(e.node, e.key, offset);
            }
        }
    }

    KeyType     width_ = 1;
    KeyType     base_  = 0;
    std::size_t ring_  = 1024;
    std::size_t mask_  = 1023;
    std::size_t cursor_ = 0;
    std::size_t size_   = 0;

    std::vector<std::uint32_t> head_;
    std::vector<Item>          pool_;
    std::vector<KeyType>       keys_;  // parallel to pool_, only when width > 1
    std::uint32_t              free_ = kNil;
    BucketBitmap               nonempty_;

    std::vector<Spilled> overflow_;
    KeyType           overflow_min_ = kNoKey;

    std::uint64_t comparisons_ = 0;
};

}  // namespace lpq

#endif  // LPQ_LPQ_POOLED_BUCKET_QUEUE_HPP
