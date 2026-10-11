// =============================================================================
// Authoritative bucket substrate for the LPQ (ESD.md section 10's
// AuthoritativeQueue, PRD.md section 5).
//
// =============================================================================
// WHY THIS STRUCTURE AND NOT A HEAP -- the central design decision
// =============================================================================
//
// The learned queue has to save real work somewhere, and it took some care to
// find a place where it can. The reasoning is recorded here because it
// determines everything else, and because the obvious designs do NOT work.
//
// Verification (see verification_engine.hpp) accepts a candidate when its key
// is at or below a LOWER BOUND on every key not examined. So the substrate must
// supply such a bound cheaply. That requirement rules out most structures:
//
//   A d-ary heap already puts the minimum at the root. There is nothing to
//   predict: reading the answer costs one load. Sourcing k candidates from a
//   heap in key order costs O(k log k) with an auxiliary structure -- MORE than
//   the single O(log n) sift-down a pop would have cost. A learned queue built
//   on a heap cannot win, and saying so plainly is more useful than discovering
//   it in a benchmark later.
//
//   A radix heap has no scan to save either. Its min-finding is fused into the
//   redistribution pass, which is already amortised O(1) per element per bit --
//   which is consistent with radix being the fastest queue measured in this
//   project so far (grid 200x200 SSSP: radix 541,500ns, binary 3.06x slower).
//
//   An unordered array has O(1) insertion and no bound at all, so every
//   extraction needs a full scan and nothing can be verified cheaply.
//
// What does work is a bucket queue with a CACHED PER-BUCKET MINIMUM:
//
//   * Buckets partition the key range. Insertion is O(1): compute an index,
//     push. No sift, no pointers, no rebalancing.
//
//   * Each bucket caches a lower bound on the keys it holds, maintained in O(1)
//     on insertion as min(cached, new key). This costs one comparison per push
//     and gives verification its bound for free.
//
//   * A classical bucket queue extracts by SCANNING the current bucket to find
//     which element holds the minimum -- O(bucket size), and the dominant cost
//     of the structure.
//
//   * The cached bound tells us the minimum VALUE without a scan. What it does
//     not tell us is WHICH element holds it. That is exactly the learnable
//     question, and answering it turns an O(bucket) scan into an O(k) check:
//     if a candidate's key equals the bound, it is provably minimal and the
//     rest of the bucket never has to be touched.
//
// So the model's job is precise and falsifiable: locate the element achieving a
// minimum whose value is already known. When it succeeds, a scan is skipped.
// When it fails, the fallback is an ordinary bucket scan, so the worst case is
// the classical bucket queue and never worse.
//
// -----------------------------------------------------------------------------
// The bound goes stale, on purpose
// -----------------------------------------------------------------------------
//
// After the minimum element is removed, the cached value is no longer the
// bucket's minimum -- it is only a lower bound on what remains. Recomputing it
// would cost the very O(bucket) scan this design exists to avoid.
//
// So it is deliberately left stale. It stays VALID (every remaining key is
// still at or above it), which is all verification requires, but it becomes
// loose, so the "key equals bound" test starts failing. A failure triggers the
// fallback scan, which refreshes the bound as a side effect. The result is an
// amortisation: occasional scans, free acceptances in between, and correctness
// throughout because a stale lower bound can only cause a REFUSAL, never a
// wrong acceptance.
//
// -----------------------------------------------------------------------------
// Monotonicity
// -----------------------------------------------------------------------------
//
// Like the radix heap, this structure assumes extracted keys never decrease --
// true for Dijkstra and for A* under a consistent heuristic, both of which this
// project guarantees elsewhere. The assumption is what allows the bucket base
// to advance and never look back. It is checked, not assumed: a push below the
// current base is a programming error and is reported as one.
// =============================================================================
#ifndef LPQ_LPQ_BUCKET_SUBSTRATE_HPP
#define LPQ_LPQ_BUCKET_SUBSTRATE_HPP

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/lpq/bucket_bitmap.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {

/// Whether a weight type can drive this substrate.
///
/// Integral only, for the same reason the radix heap is integral only: bucket
/// indexing is integer division, and doing it in floating point would make the
/// bucket a key falls into depend on rounding. Two keys that compare equal
/// could then land in different buckets, and the bound would no longer describe
/// the bucket's contents.
template <WeightType W>
[[nodiscard]] constexpr bool bucket_substrate_supports() noexcept {
    return std::is_integral_v<W>;
}

template <WeightType W>
[[nodiscard]] inline std::string bucket_substrate_unsupported_reason() {
    if constexpr (!std::is_integral_v<W>) {
        return "the bucket substrate requires an integral weight type: bucket indexing is "
               "integer division, and performing it in floating point would let two equal "
               "keys land in different buckets";
    }
    return "";
}

/// Monotone bucket queue with cached per-bucket key lower bounds.
///
/// Satisfies PriorityQueueLike with lazy deletion, so it can be benchmarked
/// against the classical queues on equal terms, and additionally exposes the
/// candidate/bound interface the LPQ needs.
template <WeightType W>
    requires(bucket_substrate_supports<W>())
class BucketSubstrate {
public:
    using KeyType = DistanceType<W>;

    /// Lazy deletion, like the radix heap: an improved key is pushed as a new
    /// entry and the old one is filtered on extraction. A real decrease-key
    /// would need a position index per node, and maintaining it would add a
    /// write to every insertion -- which is the operation this structure is
    /// fast at and must stay fast at.
    static constexpr bool kSupportsDecreaseKey = false;

    /// Number of buckets in the ring.
    ///
    /// A power of two so the index wrap is a mask rather than a division. 1024
    /// covers a span of 1024 * width above the current base; anything beyond
    /// goes to the overflow list and is re-bucketed when the base catches up.
    static constexpr std::size_t kBucketCount = 1024;
    static constexpr std::size_t kBucketMask  = kBucketCount - 1;

    /// Largest ring a caller may ask for. 2^22 buckets of vector headers is
    /// 100 MB, which is already far past where a wider bucket is the better
    /// trade; the cap exists so a configuration error cannot allocate gigabytes.
    static constexpr std::size_t kMaxRing = std::size_t{1} << 22;

    /// One queued entry. 16 bytes with a 64-bit key, so four share a cache
    /// line -- and the scan fallback is a linear walk over exactly this array,
    /// which is the access pattern caches are best at.
    ///
    /// The model score rides in what would otherwise be padding, so carrying it
    /// costs nothing at all. That matters: it is what allows the candidate
    /// window to be maintained AT PUSH TIME instead of selected by a scan at
    /// pop time, and selecting it by a scan would be the very cost the learned
    /// queue is trying to remove.
    struct Entry {
        KeyType key   = 0;
        NodeId  node  = 0;
        float   score = 0.0F;
    };

    BucketSubstrate() : BucketSubstrate(1) {}

    /// The ring size is a runtime parameter so a narrow bucket width can still
    /// cover the key range: the ring spans ring * width above the base, and a
    /// width of 1 on a road network needs a ring larger than the longest arc.
    /// It defaults to kBucketCount, which is what every earlier configuration
    /// used, so existing behaviour is unchanged unless a caller opts in.

    /// `width` is the key span each bucket covers.
    ///
    /// Width 1 makes every bucket hold a single key value, so the cached bound
    /// is always exact and the model's task is trivial -- useful as a control
    /// in the ablation, not as a production setting. Wider buckets hold more
    /// elements, which is where a scan is expensive and a correct prediction is
    /// worth something.
    explicit BucketSubstrate(KeyType width, std::size_t ring_size = kBucketCount) {
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
        buckets_.resize(ring_);
        bucket_bound_.assign(ring_, kNoBound);
        nonempty_.reset(ring_);
    }

    [[nodiscard]] std::size_t ring_size() const noexcept { return ring_; }

    // --- PriorityQueueLike ---------------------------------------------------

    /// Size of each bucket's best-scoring front.
    ///
    /// Zero disables front maintenance entirely, which is the classical bucket
    /// queue and the configuration the baselines are measured in.
    void set_front_size(std::size_t k) noexcept { front_k_ = k; }

    [[nodiscard]] std::size_t front_size() const noexcept { return front_k_; }

    void push(NodeId node, KeyType key) { push(node, key, 0.0F); }

    void push(NodeId node, KeyType key, float score) {
        if (key < base_) {
            // Monotonicity violated. Reported rather than tolerated: a key
            // below the base belongs to a bucket that has already been passed,
            // so it would never be extracted and the search would silently
            // return a wrong answer.
            throw InternalError(
                "bucket substrate received a key below the current base (" +
                std::to_string(static_cast<std::uint64_t>(key)) + " < " +
                std::to_string(static_cast<std::uint64_t>(base_)) +
                "); extracted keys must be non-decreasing");
        }

        const std::size_t offset = static_cast<std::size_t>((key - base_) / width_);
        ++size_;

        if (offset >= ring_) {
            overflow_.push_back(Entry{key, node, score});
            if (overflow_bound_ == kNoBound || key < overflow_bound_) {
                overflow_bound_ = key;
            }
            return;
        }

        const std::size_t index = (cursor_ + offset) & mask_;
        insert_into_bucket(buckets_[index], Entry{key, node, score});
        nonempty_.set(index);

        // The O(1) bound maintenance that makes cheap verification possible.
        ++comparisons_;
        if (bucket_bound_[index] == kNoBound || key < bucket_bound_[index]) {
            bucket_bound_[index] = key;
        }
    }

    /// Classical extraction: advance to the first non-empty bucket, scan it.
    ///
    /// This is both the standalone queue's pop and the LPQ's fallback path, so
    /// the LPQ's worst case is exactly the classical bucket queue.
    [[nodiscard]] QueueEntry<KeyType> pop() {
        const std::size_t at = locate_minimum();
        std::vector<Entry>& bucket = buckets_[cursor_];

        const Entry taken = bucket[at];
        bucket[at]        = bucket.back();
        bucket.pop_back();
        --size_;
        if (bucket.empty()) {
            nonempty_.clear(cursor_);
        }

        // The bound is NOT recomputed here. See the header: refreshing it would
        // cost the scan this structure exists to avoid, and a stale lower bound
        // can only cause a refusal, never a wrong acceptance.
        return QueueEntry<KeyType>{taken.node, taken.key};
    }

    /// The minimum without removing it.
    ///
    /// Genuinely const: it walks the ring on a local cursor rather than
    /// advancing the member one, and never refreshes a cached bound. A
    /// const_cast here would be observable -- top() would move the structure's
    /// traversal state and mutate bounds -- and the concept hands this function
    /// a const reference precisely so callers can rely on it not doing that.
    ///
    /// The cost is that top() cannot amortise the way pop() does, so it is
    /// O(bucket) every time. Dijkstra here calls pop(), not top(), so this is
    /// the cold path; it exists to satisfy PriorityQueueLike and to give the
    /// differential tests something to compare.
    [[nodiscard]] QueueEntry<KeyType> top() const {
        if (size_ == 0) {
            throw InternalError("top() called on an empty bucket substrate");
        }

        for (std::size_t step = 0; step < ring_; ++step) {
            const std::vector<Entry>& bucket = buckets_[(cursor_ + step) & mask_];
            if (bucket.empty()) {
                continue;
            }
            const Entry* best = &bucket.front();
            for (const Entry& e : bucket) {
                ++comparisons_;
                if (e.key < best->key || (e.key == best->key && e.node < best->node)) {
                    best = &e;
                }
            }
            return QueueEntry<KeyType>{best->node, best->key};
        }

        // Everything left is beyond the ring's span.
        const Entry* best = &overflow_.front();
        for (const Entry& e : overflow_) {
            ++comparisons_;
            if (e.key < best->key || (e.key == best->key && e.node < best->node)) {
                best = &e;
            }
        }
        return QueueEntry<KeyType>{best->node, best->key};
    }

    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    void clear() noexcept {
        // Visit only the buckets that hold something: a query that touched a few
        // hundred of a 65536-bucket ring should not pay to clear all of it.
        for (std::size_t i = nonempty_.find_next(0); i != BucketBitmap::npos;
             i = nonempty_.find_next(i + 1)) {
            buckets_[i].clear();
        }
        nonempty_.clear_all();
        std::fill(bucket_bound_.begin(), bucket_bound_.end(), kNoBound);
        overflow_.clear();
        overflow_bound_ = kNoBound;
        size_           = 0;
        cursor_         = 0;
        base_           = 0;
    }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept { return comparisons_; }
    void reset_comparison_count() noexcept { comparisons_ = 0; }

    void reserve_nodes(std::size_t /*node_count*/) {
        // Deliberately does nothing. Reserving every bucket for the whole node
        // count would allocate node_count * kBucketCount entries -- gigabytes
        // on a large graph -- for a structure whose occupancy is spread thinly
        // across buckets. The per-bucket vectors grow geometrically instead.
    }

    // --- LPQ interface -------------------------------------------------------

    /// The entries in the bucket currently holding the minimum.
    ///
    /// Advances the cursor past empty buckets as a side effect, which is why it
    /// is non-const. Empty only when the queue is empty.
    [[nodiscard]] std::span<const Entry> current_bucket() {
        if (size_ == 0) {
            return {};
        }
        advance_to_non_empty();
        return {buckets_[cursor_].data(), buckets_[cursor_].size()};
    }

    /// The best-scoring entries of the current bucket: the LPQ's candidate
    /// window, available without examining the rest of the bucket.
    ///
    /// The front is BEST EFFORT, not an exact top-k. A removal from inside it
    /// is repaired by swapping in whatever sits at the back, which can leave a
    /// lower-scoring entry in the front and a higher-scoring one outside it.
    ///
    /// That looseness is deliberate and safe. The front only decides which
    /// candidates are LOOKED AT; whether one may be extracted is decided by
    /// verification against the bound, which does not consult scores at all. So
    /// a degraded front costs speed -- more refusals, more fallback scans --
    /// and can never cost correctness. Keeping it exact would mean an O(bucket)
    /// repair on every removal, which is the cost this whole design exists to
    /// avoid.
    [[nodiscard]] std::span<const Entry> current_front() {
        if (size_ == 0) {
            return {};
        }
        advance_to_non_empty();
        const std::vector<Entry>& bucket = buckets_[cursor_];
        return {bucket.data(), std::min(front_k_, bucket.size())};
    }

    /// A valid lower bound on every key still in the queue.
    ///
    /// This is what the LPQ hands to verification. It is the cached bound of
    /// the current bucket, which may be stale (loose) but is never wrong: no
    /// key in this bucket or any later one falls below it.
    [[nodiscard]] KeyType current_bound() {
        if (size_ == 0) {
            return 0;
        }
        advance_to_non_empty();
        const KeyType cached = bucket_bound_[cursor_];
        // The bucket's structural lower edge is also a valid bound, and is
        // tighter than a badly stale cache after many removals.
        const KeyType structural = base_;
        return (cached == kNoBound || cached < structural) ? structural : cached;
    }

    /// Removes a specific entry from the current bucket by node id.
    ///
    /// Returns false if the node is not present, which the LPQ treats as a
    /// programming error rather than a normal outcome -- it only ever removes
    /// a node that verification just accepted from this same bucket.
    bool remove_from_current_bucket(NodeId node, KeyType key) {
        if (size_ == 0) {
            return false;
        }
        advance_to_non_empty();
        std::vector<Entry>& bucket = buckets_[cursor_];
        for (std::size_t i = 0; i < bucket.size(); ++i) {
            if (bucket[i].node == node && bucket[i].key == key) {
                bucket[i] = bucket.back();
                bucket.pop_back();
                --size_;
                if (bucket.empty()) {
                    nonempty_.clear(cursor_);
                }
                return true;
            }
        }
        return false;
    }

    // --- Direct extraction (LearnedPriorityQueue, ExtractionStrategy::kDirect) --
    //
    // The modular path copies a front into the CandidateManager, verifies the
    // copy, and then searches the bucket again to remove the winner. These
    // accessors let a caller verify IN PLACE and remove by the index it already
    // holds, so none of that copying or re-searching happens. They expose the
    // current bucket's storage and nothing else: the bound, the size counter and
    // the cursor stay owned by the substrate, which is what keeps the invariants
    // (bound <= every key in the bucket; size_ == contents) in one place.

    /// The current bucket, advanced to first, for in-place inspection.
    /// Precondition: the queue is not empty.
    [[nodiscard]] std::span<const Entry> peek_current_bucket() {
        advance_to_non_empty();
        const std::vector<Entry>& b = buckets_[cursor_];
        return {b.data(), b.size()};
    }

    /// Removes the entry at `index` of the current bucket by swapping the last
    /// entry into its slot. O(1). The caller obtained `index` from
    /// peek_current_bucket() with no push in between.
    ///
    /// Does not touch the cached bound: a removal can only leave it loose, never
    /// invalid.
    QueueEntry<KeyType> take_from_current(std::size_t index) {
        std::vector<Entry>& bucket = buckets_[cursor_];
        const Entry         taken  = bucket[index];
        bucket[index]              = bucket.back();
        bucket.pop_back();
        --size_;
        if (bucket.empty()) {
            nonempty_.clear(cursor_);
        }
        return QueueEntry<KeyType>{taken.node, taken.key};
    }

    /// Records that a scan of the whole current bucket found `exact_minimum`.
    /// Only a full scan may call this: the bound must stay a lower bound.
    void set_current_bound(KeyType exact_minimum) noexcept {
        bucket_bound_[cursor_] = exact_minimum;
    }

    /// Adds comparisons performed by a caller working on the bucket in place, so
    /// comparison_count() stays the whole queue's work and not just the push side.
    void add_comparisons(std::uint64_t n) noexcept { comparisons_ += n; }

    /// Recomputes the current bucket's bound with a full scan, and reports the
    /// index of the minimum.
    ///
    /// The fallback path, and the only thing that tightens a stale bound.
    [[nodiscard]] std::size_t rescan_current_bucket() { return locate_minimum(); }

    /// Entries in the current bucket. The quantity a correct prediction saves a
    /// scan over, so it is what the experiment reports to show the saving.
    [[nodiscard]] std::size_t current_bucket_size() {
        if (size_ == 0) {
            return 0;
        }
        advance_to_non_empty();
        return buckets_[cursor_].size();
    }

    [[nodiscard]] KeyType width() const noexcept { return width_; }
    [[nodiscard]] KeyType base() const noexcept { return base_; }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        std::size_t bytes = bucket_bound_.capacity() * sizeof(KeyType);
        for (const std::vector<Entry>& b : buckets_) {
            bytes += b.capacity() * sizeof(Entry);
        }
        return bytes + overflow_.capacity() * sizeof(Entry) + nonempty_.memory_bytes();
    }

private:
    /// Sentinel for "no bound recorded". The maximum key can never be a real
    /// bound in practice, and using it avoids a parallel array of flags.
    static constexpr KeyType kNoBound = std::numeric_limits<KeyType>::max();

    /// Moves the cursor forward to a bucket that holds something.
    ///
    /// Each step advances the base by one bucket width, so the total work over
    /// a whole search is bounded by the key range divided by the width rather
    /// than by the number of extractions.
    ///
    /// Invariant maintained here: every overflow key lies at or beyond the end
    /// of the window, base + kBucketCount * width. The window slides forward
    /// as the base advances, and an overflow entry the window now covers is
    /// re-filed into its bucket at that moment. Without this, an entry parked
    /// in the overflow list while the window was further back stayed there as
    /// the window caught up, later pushes with LARGER keys landed in buckets,
    /// and extraction returned them first -- non-optimal distances in exact
    /// mode whenever the bucket width was too small for the key range (found
    /// by fuzz_queue_ops; regression tests in test_bucket_substrate.cpp).
    void advance_to_non_empty() {
        absorb_overflow();
        if (!buckets_[cursor_].empty()) {
            return;  // the common case: still inside the bucket being drained
        }
        for (;;) {
            // Jump straight to the next occupied bucket in ring order. The cost
            // is independent of the gap, which is what makes narrow buckets
            // affordable.
            std::size_t next = nonempty_.find_next(cursor_);
            if (next == BucketBitmap::npos) {
                next = nonempty_.find_next(0);
            }
            if (next == BucketBitmap::npos) {
                // The ring is empty, so everything left is in the overflow list.
                redistribute_overflow();
                continue;
            }
            const std::size_t steps = (next - cursor_) & mask_;
            // Reset the bound of the bucket being LEFT. When next == cursor_ --
            // which happens right after redistribute_overflow() has refilled the
            // bucket the cursor already sits on -- that bucket is the one being
            // ARRIVED at, its bound was just recorded by push(), and wiping it
            // lets a later push record a bound above a key already queued.
            if (next != cursor_) {
                bucket_bound_[cursor_] = kNoBound;
            }
            cursor_                 = next;
            base_ += static_cast<KeyType>(steps) * width_;
            // The window moved. Anything it now covers is later than the bucket
            // just reached, so re-filing it cannot change which bucket is first.
            absorb_overflow();
            return;
        }
    }

    /// Re-files every overflow entry the window now covers (see above).
    ///
    /// O(1) when nothing is covered, which is every call in a configuration
    /// whose width spans the key range; otherwise one pass over the overflow
    /// list. Each entry leaves the list once.
    /// Returns whether any entry was re-filed.
    bool absorb_overflow() {
        if (overflow_bound_ == kNoBound) {
            return false;
        }
        const KeyType window_end = base_ + static_cast<KeyType>(ring_) * width_;
        if (overflow_bound_ >= window_end) {
            return false;
        }
        std::vector<Entry> pending;
        pending.swap(overflow_);
        overflow_bound_ = kNoBound;
        for (const Entry& e : pending) {
            if (e.key < window_end) {
                --size_;  // push() re-counts it
                push(e.node, e.key, e.score);
            } else {
                overflow_.push_back(e);
                if (overflow_bound_ == kNoBound || e.key < overflow_bound_) {
                    overflow_bound_ = e.key;
                }
            }
        }
        return true;
    }

    /// Scans the current bucket, refreshes its bound, and returns the index of
    /// the minimum entry.
    [[nodiscard]] std::size_t locate_minimum() {
        if (size_ == 0) {
            throw InternalError("pop() or top() called on an empty bucket substrate");
        }
        advance_to_non_empty();

        const std::vector<Entry>& bucket = buckets_[cursor_];
        std::size_t               best   = 0;
        for (std::size_t i = 1; i < bucket.size(); ++i) {
            ++comparisons_;
            if (bucket[i].key < bucket[best].key ||
                (bucket[i].key == bucket[best].key && bucket[i].node < bucket[best].node)) {
                best = i;
            }
        }
        // The scan has just established the exact minimum, so the bound is
        // refreshed for free. This is what makes the staleness amortise.
        bucket_bound_[cursor_] = bucket[best].key;
        return best;
    }

    /// Re-files overflow entries once the base has advanced far enough.
    void redistribute_overflow() {
        if (overflow_.empty()) {
            throw InternalError(
                "bucket substrate is non-empty but every bucket and the overflow list are "
                "empty; the size counter and the contents have diverged");
        }

        // Jump the base straight to the smallest overflow key rather than
        // stepping a bucket at a time: with a wide key range the gap can be
        // enormous, and stepping it would be a loop over the range rather than
        // over the data.
        const KeyType lowest = overflow_bound_;
        base_                = lowest - (lowest % width_);
        cursor_              = 0;
        std::fill(bucket_bound_.begin(), bucket_bound_.end(), kNoBound);
        nonempty_.clear_all();  // already empty: this runs only when the ring is

        std::vector<Entry> pending;
        pending.swap(overflow_);
        overflow_bound_ = kNoBound;

        const std::size_t moved = pending.size();
        size_ -= moved;  // push() re-counts them
        for (const Entry& e : pending) {
            // The score is carried over: re-filing without it would silently
            // reset every overflowed entry's learned score to zero.
            push(e.node, e.key, e.score);
        }
    }

    KeyType     width_ = 1;
    KeyType     base_  = 0;
    std::size_t ring_  = kBucketCount;
    std::size_t mask_  = kBucketMask;
    BucketBitmap nonempty_;

    std::vector<std::vector<Entry>> buckets_;
    std::vector<KeyType>            bucket_bound_;
    std::vector<Entry>              overflow_;
    KeyType                         overflow_bound_ = kNoBound;

    /// Places an entry, keeping the bucket's best-scoring front at the front.
    ///
    /// O(1) for the common case -- an entry that does not beat the front simply
    /// appends. Only an entry good enough to enter the front pays the O(k)
    /// shift, and k is small.
    void insert_into_bucket(std::vector<Entry>& bucket, const Entry& e) {
        if (front_k_ == 0) {
            bucket.push_back(e);
            return;
        }

        if (bucket.size() < front_k_) {
            // The front is not yet full, so this entry belongs in it. Insert in
            // descending score order.
            std::size_t at = bucket.size();
            bucket.push_back(e);
            while (at > 0 && bucket[at - 1].score < e.score) {
                bucket[at] = bucket[at - 1];
                --at;
            }
            bucket[at] = e;
            return;
        }

        if (e.score <= bucket[front_k_ - 1].score) {
            bucket.push_back(e);  // not good enough for the front
            return;
        }

        // Displace the weakest front entry to the tail, then shift the new one
        // into place.
        bucket.push_back(bucket[front_k_ - 1]);
        std::size_t at = front_k_ - 1;
        while (at > 0 && bucket[at - 1].score < e.score) {
            bucket[at] = bucket[at - 1];
            --at;
        }
        bucket[at] = e;
    }

    std::size_t   front_k_     = 0;
    std::size_t   cursor_      = 0;
    std::size_t   size_        = 0;
    /// Mutable so the genuinely const top() can still count its own work:
    /// the count is instrumentation, not part of the queue's value.
    mutable std::uint64_t comparisons_ = 0;
};

}  // namespace lpq

#endif  // LPQ_LPQ_BUCKET_SUBSTRATE_HPP
