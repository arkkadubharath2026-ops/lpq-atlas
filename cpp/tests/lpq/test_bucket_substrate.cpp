// =============================================================================
// Bucket substrate (ESD.md section 10's AuthoritativeQueue).
//
// Two things are being established here.
//
// First, that it is a correct priority queue: it is differentially compared
// against the binary heap over thousands of random monotone workloads, because
// the LPQ's fallback path IS this queue and a bug in it would be a wrong
// answer, not a slow one.
//
// Second, and specific to this structure, that the CACHED BOUND is always a
// valid lower bound on everything still queued -- including when it is stale.
// Verification's soundness rests entirely on that, so it is checked directly
// rather than inferred from the queue behaving correctly.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/lpq/bucket_substrate.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace lpq {
namespace {

using Substrate = BucketSubstrate<std::uint32_t>;

static_assert(PriorityQueueLike<Substrate>);
static_assert(LazyDeletionQueue<Substrate>);

// Floating-point weights are refused at compile time, with a reason a caller
// can act on.
static_assert(!bucket_substrate_supports<double>());
static_assert(bucket_substrate_supports<std::uint32_t>());
static_assert(bucket_substrate_supports<std::uint64_t>());

// -----------------------------------------------------------------------------
// Basic queue behaviour
// -----------------------------------------------------------------------------

TEST(BucketSubstrate, StartsEmpty) {
    Substrate q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);
    EXPECT_THROW((void)q.pop(), InternalError);
    EXPECT_THROW((void)q.top(), InternalError);
}

TEST(BucketSubstrate, RejectsAZeroWidth) {
    EXPECT_THROW(Substrate{0U}, InvalidConfigError);
}

TEST(BucketSubstrate, PopsInKeyOrder) {
    Substrate q(4);
    q.push(1, 30);
    q.push(2, 10);
    q.push(3, 20);

    EXPECT_EQ(q.pop().key, 10U);
    EXPECT_EQ(q.pop().key, 20U);
    EXPECT_EQ(q.pop().key, 30U);
    EXPECT_TRUE(q.empty());
}

TEST(BucketSubstrate, TopDoesNotRemoveAndDoesNotDisturbTheQueue) {
    Substrate q(4);
    q.push(1, 30);
    q.push(2, 10);

    EXPECT_EQ(q.top().node, 2U);
    EXPECT_EQ(q.top().node, 2U);  // repeatable
    EXPECT_EQ(q.size(), 2U);
    EXPECT_EQ(q.pop().node, 2U);
}

TEST(BucketSubstrate, HandlesDuplicateKeysDeterministically) {
    // Lazy deletion means the same node can be queued more than once, and equal
    // keys are common. Ties break on node id so extraction order does not
    // depend on insertion order.
    Substrate forward(8);
    forward.push(7, 5);
    forward.push(3, 5);

    Substrate backward(8);
    backward.push(3, 5);
    backward.push(7, 5);

    EXPECT_EQ(forward.pop().node, 3U);
    EXPECT_EQ(backward.pop().node, 3U);
}

TEST(BucketSubstrate, RefusesAKeyBelowTheBase) {
    // A key under the base belongs to a bucket already passed, so it would
    // never be extracted and the search would silently return a wrong answer.
    Substrate q(4);
    q.push(1, 100);
    (void)q.pop();
    EXPECT_THROW(q.push(2, 0), InternalError);
}

TEST(BucketSubstrate, ClearResetsEverything) {
    Substrate q(4);
    q.push(1, 100);
    q.push(2, 200);
    (void)q.pop();

    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.base(), 0U);
    EXPECT_NO_THROW(q.push(3, 0));  // base is back to zero
}

TEST(BucketSubstrate, HandlesKeysBeyondTheRingSpan) {
    // The ring covers kBucketCount * width above the base; anything further
    // goes to the overflow list and must come back in the right order.
    Substrate q(1);
    const std::uint64_t far = Substrate::kBucketCount * 10;

    q.push(1, static_cast<std::uint64_t>(far));
    q.push(2, 5);
    q.push(3, static_cast<std::uint64_t>(far) * 2);

    EXPECT_EQ(q.pop().node, 2U);
    EXPECT_EQ(q.pop().node, 1U);
    EXPECT_EQ(q.pop().node, 3U);
    EXPECT_TRUE(q.empty());
}

// Regression: fuzz_queue_ops, random stream seed 20260926 (LPQ Atlas). The
// test above only reaches the overflow list after a full empty lap. Here the
// window slides forward WITHOUT an empty lap until it covers the parked
// overflow key, and a later, larger key is pushed into a bucket. The substrate
// returned 1600 before 1500 -- in a search, a non-optimal distance in exact
// mode.
TEST(BucketSubstrate, AnOverflowKeyTheWindowCatchesUpWithIsNotOvertaken) {
    Substrate q(1);
    q.push(1, 0);
    q.push(2, 1500);  // offset 1500 >= kBucketCount: parked in the overflow list
    EXPECT_EQ(q.pop().key, 0U);
    q.push(3, 600);
    EXPECT_EQ(q.pop().key, 600U);  // the window now ends at 600 + 1024 > 1500
    q.push(4, 1600);               // lands in a bucket
    EXPECT_EQ(q.pop().key, 1500U);
    EXPECT_EQ(q.pop().key, 1600U);
    EXPECT_TRUE(q.empty());
}

TEST(BucketSubstrate, RefilingFromTheOverflowListKeepsTheScore) {
    Substrate q(1);
    q.set_front_size(4);
    q.push(1, 0, 0.1F);
    q.push(2, 5000, 0.9F);  // overflows
    EXPECT_EQ(q.pop().key, 0U);
    const auto front = q.current_front();  // re-files the overflow entry
    ASSERT_EQ(front.size(), 1U);
    EXPECT_EQ(front[0].node, 2U);
    EXPECT_FLOAT_EQ(front[0].score, 0.9F);
}

TEST(BucketSubstrate, StaysOrderedWhenTheWidthIsFarBelowTheKeyRange) {
    // Dijkstra's monotone usage with keys jumping far beyond the window, for
    // widths that force the overflow path constantly.
    for (const std::uint64_t width : {1U, 2U, 3U, 7U, 16U}) {
        for (std::uint64_t seed = 1; seed <= 30; ++seed) {
            Rng                        rng(seed * 7919 + width);
            Substrate                  q(width);
            std::vector<std::uint64_t> ref;  // kept sorted
            std::uint64_t              floor = 0;
            for (int i = 0; i < 3000; ++i) {
                if (!ref.empty() && rng.next_unit() < 0.45) {
                    const auto e = q.pop();
                    ASSERT_EQ(e.key, ref.front()) << "width " << width << " seed " << seed;
                    ref.erase(ref.begin());
                    floor = e.key;
                } else {
                    const std::uint64_t key = floor + rng.next_in_range(0, 20000);
                    q.push(static_cast<NodeId>(i), key);
                    ref.insert(std::upper_bound(ref.begin(), ref.end(), key), key);
                }
                ASSERT_EQ(q.size(), ref.size());
            }
            while (!ref.empty()) {
                ASSERT_EQ(q.pop().key, ref.front());
                ref.erase(ref.begin());
            }
        }
    }
}

TEST(BucketSubstrate, JumpsAcrossAHugeEmptyKeyRange) {
    // Stepping a bucket at a time across a sparse key range would be a loop
    // over the range rather than over the data.
    Substrate q(1);
    q.push(1, 0);
    q.push(2, 3000000000ULL);

    EXPECT_EQ(q.pop().node, 1U);
    EXPECT_EQ(q.pop().node, 2U);
}

// -----------------------------------------------------------------------------
// The bound: the property verification depends on
// -----------------------------------------------------------------------------

TEST(BucketSubstrate, BoundIsExactAfterInsertion) {
    Substrate q(100);
    q.push(1, 30);
    q.push(2, 10);
    q.push(3, 20);
    EXPECT_EQ(q.current_bound(), 10U);
}

TEST(BucketSubstrate, BoundGoesStaleAfterRemovalButStaysValid) {
    // Refreshing it would cost the scan this design exists to avoid, so the
    // cached value is left behind deliberately. It must remain a LOWER bound.
    Substrate q(100);
    q.push(1, 10);
    q.push(2, 20);
    q.push(3, 30);
    ASSERT_EQ(q.current_bound(), 10U);

    ASSERT_TRUE(q.remove_from_current_bucket(1, 10));

    const std::uint64_t bound = q.current_bound();
    EXPECT_LE(bound, 20U) << "the stale bound must never exceed the true minimum";
    EXPECT_EQ(q.top().key, 20U);
}

TEST(BucketSubstrate, AScanRefreshesTheBound) {
    Substrate q(100);
    q.push(1, 10);
    q.push(2, 20);
    q.push(3, 30);
    ASSERT_TRUE(q.remove_from_current_bucket(1, 10));

    (void)q.rescan_current_bucket();
    EXPECT_EQ(q.current_bound(), 20U);  // tightened for free by the scan
}

// The invariant everything rests on, checked continuously against brute force
// over a long randomised workload.
TEST(BucketSubstrate, BoundIsAlwaysAValidLowerBoundOnEverythingQueued) {
    Rng rng(20260922U);

    for (int trial = 0; trial < 300; ++trial) {
        const std::uint64_t width = 1 + rng.next_below(64);
        Substrate           q(width);

        std::vector<std::uint64_t> live;  // every key still in the queue

        for (int step = 0; step < 300; ++step) {
            const bool do_push = live.empty() || rng.next_below(3) != 0;

            if (do_push) {
                // Pushed relative to the QUEUE's base, not to the last key
                // popped. Inspecting the bound advances the base past empty
                // buckets, so the last popped key can sit below it, and a
                // caller that used it as the floor would violate monotonicity
                // without doing anything a real search would not do.
                const std::uint64_t key = q.base() + rng.next_below(500);
                q.push(static_cast<NodeId>(step), key);
                live.push_back(key);
            } else {
                const auto popped = q.pop();
                live.erase(std::find(live.begin(), live.end(), popped.key));
            }

            if (!live.empty()) {
                const std::uint64_t bound = q.current_bound();
                const std::uint64_t true_min = *std::min_element(live.begin(), live.end());
                ASSERT_LE(bound, true_min)
                    << "trial " << trial << " step " << step
                    << ": the bound exceeded the true minimum, which would let "
                       "verification accept a non-minimal node";
            }
        }
    }
}

TEST(BucketSubstrate, CurrentBucketHoldsTheMinimum) {
    Rng rng(4242U);

    for (int trial = 0; trial < 500; ++trial) {
        Substrate q(1 + rng.next_below(32));

        std::vector<std::uint64_t> keys;
        const std::size_t          n = 1 + static_cast<std::size_t>(rng.next_below(40));
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t key = rng.next_below(2000);
            q.push(static_cast<NodeId>(i), key);
            keys.push_back(key);
        }

        const std::uint64_t true_min = *std::min_element(keys.begin(), keys.end());
        const auto          bucket   = q.current_bucket();

        ASSERT_FALSE(bucket.empty()) << "trial " << trial;
        const auto found = std::min_element(
            bucket.begin(), bucket.end(),
            [](const Substrate::Entry& a, const Substrate::Entry& b) { return a.key < b.key; });
        EXPECT_EQ(found->key, true_min) << "trial " << trial;
    }
}

TEST(BucketSubstrate, RemoveReportsAMissingNodeRatherThanCorruptingTheSize) {
    Substrate q(100);
    q.push(1, 10);
    EXPECT_FALSE(q.remove_from_current_bucket(99, 10));
    EXPECT_FALSE(q.remove_from_current_bucket(1, 999));
    EXPECT_EQ(q.size(), 1U);
}

// -----------------------------------------------------------------------------
// Differential correctness against the binary heap
//
// The LPQ's fallback path is this queue, so a bug here is a wrong answer rather
// than a slow one.
// -----------------------------------------------------------------------------

TEST(BucketSubstrate, MatchesTheBinaryHeapOnRandomMonotoneWorkloads) {
    Rng rng(1002U);  // the differential regression seed

    for (int trial = 0; trial < 2000; ++trial) {
        const std::uint64_t width = 1 + rng.next_below(128);

        Substrate                               substrate(width);
        BinaryHeap<DistanceType<std::uint32_t>> heap;

        // The binary heap indexes a position map by node id, so it must be
        // sized before use. The substrate deliberately ignores reserve_nodes --
        // reserving every bucket for the whole node count would allocate
        // node_count * kBucketCount entries -- which is why the two are sized
        // differently here rather than by a shared helper.
        constexpr NodeId kNodeCount = 256;
        heap.reserve_nodes(kNodeCount);
        substrate.reserve_nodes(kNodeCount);

        std::uint64_t base    = 0;
        NodeId        next_id = 0;

        for (int step = 0; step < 120; ++step) {
            const bool do_push = substrate.empty() || rng.next_below(3) != 0;

            if (do_push) {
                ASSERT_LT(next_id, kNodeCount) << "the heap's position map would overflow";
                // Keys are drawn from a narrow band above the last extraction,
                // which is what a real Dijkstra frontier looks like and what
                // keeps the workload monotone.
                const std::uint64_t key = base + rng.next_below(200);
                substrate.push(next_id, key);
                heap.push(next_id, key);
                ++next_id;
            } else {
                const auto a = substrate.pop();
                const auto b = heap.pop();

                // Keys must agree exactly. Node ids may differ when keys tie,
                // because the two structures break ties on different internal
                // orders -- and any node holding the minimum key is a correct
                // extraction, so demanding identity would fail a correct queue
                // for being right in a different permissible way.
                ASSERT_EQ(a.key, b.key) << "trial " << trial << " step " << step;
                base = a.key;
            }

            ASSERT_EQ(substrate.size(), heap.size()) << "trial " << trial << " step " << step;
        }

        // Drain both and compare the full remaining key sequence.
        while (!substrate.empty()) {
            ASSERT_EQ(substrate.pop().key, heap.pop().key) << "trial " << trial;
        }
        EXPECT_TRUE(heap.empty());
    }
}

TEST(BucketSubstrate, MatchesTheBinaryHeapWhenEveryKeyIsIdentical) {
    // Degenerate but realistic: an unweighted graph puts the whole frontier in
    // one bucket, which is the worst case for the scan and the best case for a
    // correct prediction.
    Substrate                               substrate(16);
    BinaryHeap<DistanceType<std::uint32_t>> heap;
    heap.reserve_nodes(500);

    for (NodeId u = 0; u < 500; ++u) {
        substrate.push(u, 7);
        heap.push(u, 7);
    }
    while (!substrate.empty()) {
        ASSERT_EQ(substrate.pop().key, heap.pop().key);
    }
}

// -----------------------------------------------------------------------------
// Instrumentation
// -----------------------------------------------------------------------------

TEST(BucketSubstrate, CountsComparisonsAndResetsThem) {
    Substrate q(100);
    q.push(1, 10);
    q.push(2, 20);
    q.push(3, 30);
    (void)q.pop();

    EXPECT_GT(q.comparison_count(), 0U);
    q.reset_comparison_count();
    EXPECT_EQ(q.comparison_count(), 0U);
}

// A wide bucket makes the scan expensive, which is exactly the cost a correct
// prediction removes. This records that the saving being targeted is real and
// scales with bucket occupancy.
TEST(BucketSubstrate, ScanCostGrowsWithBucketOccupancy) {
    auto comparisons_to_pop = [](std::uint64_t width, NodeId count) {
        Substrate q(width);
        for (NodeId u = 0; u < count; ++u) {
            q.push(u, 1 + (u % 8));
        }
        q.reset_comparison_count();
        (void)q.pop();
        return q.comparison_count();
    };

    const std::uint64_t narrow = comparisons_to_pop(1, 200);
    const std::uint64_t wide   = comparisons_to_pop(64, 200);

    EXPECT_LT(narrow, wide)
        << "a wider bucket must cost more to scan; narrow " << narrow << ", wide " << wide;
}

}  // namespace
}  // namespace lpq
