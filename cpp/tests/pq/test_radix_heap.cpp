// =============================================================================
// Radix heap tests (ESD.md section 7).
//
// Tested separately from the shared typed suite because its contract genuinely
// differs: it is a lazy-deletion queue, so it may hold several entries per node
// and offers no decrease_key or contains; and it requires monotone keys, a
// precondition none of the comparison heaps impose.
//
// The monotone requirement is exercised in the way Dijkstra actually produces
// keys -- each pushed key is the last extracted key plus a non-negative
// increment -- rather than by pushing arbitrary sorted values, so that the test
// reflects the structure's real usage.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <queue>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/pq/priority_queue.hpp"
#include "lpq/pq/radix_heap.hpp"

namespace {

using lpq::NodeId;
using lpq::RadixHeap;
using lpq::Rng;

using Key = std::uint64_t;

TEST(RadixHeap, SatisfiesThePriorityQueueConcept) {
    static_assert(lpq::PriorityQueueLike<RadixHeap<Key>>);
    // It must NOT claim decrease-key support; Dijkstra branches on this to
    // decide whether stale entries need filtering.
    static_assert(!RadixHeap<Key>::kSupportsDecreaseKey);
    static_assert(lpq::LazyDeletionQueue<RadixHeap<Key>>);
    SUCCEED();
}

TEST(RadixHeap, StartsEmpty) {
    RadixHeap<Key> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);
}

TEST(RadixHeap, SingleElementRoundTrips) {
    RadixHeap<Key> q;
    q.push(3, 42);
    EXPECT_EQ(q.size(), 1U);
    EXPECT_EQ(q.top().node, 3U);
    const auto e = q.pop();
    EXPECT_EQ(e.node, 3U);
    EXPECT_EQ(e.key, 42U);
    EXPECT_TRUE(q.empty());
}

TEST(RadixHeap, PopsInNonDecreasingOrder) {
    RadixHeap<Key> q;
    // Pushed out of order, but all at or above last_extracted() == 0.
    const std::vector<Key> keys{50, 3, 17, 99, 1, 64, 8};
    for (std::uint32_t i = 0; i < keys.size(); ++i) {
        q.push(i, keys[i]);
    }
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    for (Key expected : sorted) {
        EXPECT_EQ(q.pop().key, expected);
    }
    EXPECT_TRUE(q.empty());
}

TEST(RadixHeap, TracksTheLastExtractedKey) {
    RadixHeap<Key> q;
    EXPECT_EQ(q.last_extracted(), 0U);
    q.push(0, 10);
    q.push(1, 20);
    q.pop();
    EXPECT_EQ(q.last_extracted(), 10U);
}

TEST(RadixHeap, HandlesDuplicateKeys) {
    RadixHeap<Key> q;
    for (std::uint32_t i = 0; i < 20; ++i) {
        q.push(i, 7);
    }
    std::vector<NodeId> seen;
    while (!q.empty()) {
        const auto e = q.pop();
        EXPECT_EQ(e.key, 7U);
        seen.push_back(e.node);
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen.size(), 20U);
    EXPECT_TRUE(std::adjacent_find(seen.begin(), seen.end()) == seen.end())
        << "every node must be returned exactly once";
}

TEST(RadixHeap, HandlesTheSameNodePushedTwice) {
    // The lazy-deletion pattern: an improved key is pushed as a new entry
    // rather than updating the old one. Both entries come out, and Dijkstra
    // discards the stale one.
    RadixHeap<Key> q;
    q.push(5, 100);
    q.push(5, 30);
    EXPECT_EQ(q.size(), 2U) << "lazy deletion keeps both entries";
    EXPECT_EQ(q.pop().key, 30U) << "the better key must come out first";
    EXPECT_EQ(q.pop().key, 100U);
}

TEST(RadixHeap, HandlesZeroKeys) {
    RadixHeap<Key> q;
    for (std::uint32_t i = 0; i < 10; ++i) {
        q.push(i, 0);
    }
    for (std::uint32_t i = 0; i < 10; ++i) {
        EXPECT_EQ(q.pop().key, 0U);
    }
}

TEST(RadixHeap, HandlesWidelySeparatedKeys) {
    // Exercises the high bucket indices: bucket_for uses bit_width of the XOR,
    // so keys differing in the top bit land in the last bucket.
    RadixHeap<Key> q;
    q.push(0, 1);
    q.push(1, 1ULL << 40);
    q.push(2, 1ULL << 62);
    EXPECT_EQ(q.pop().key, 1U);
    EXPECT_EQ(q.pop().key, 1ULL << 40);
    EXPECT_EQ(q.pop().key, 1ULL << 62);
}

TEST(RadixHeap, ClearAllowsReuse) {
    RadixHeap<Key> q;
    for (std::uint32_t i = 0; i < 10; ++i) {
        q.push(i, 100 + i);
    }
    q.pop();
    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.last_extracted(), 0U)
        << "clear must reset the monotone reference point, or later low keys "
           "would land in already-drained buckets";

    q.push(0, 5);
    EXPECT_EQ(q.pop().key, 5U);
}

// --- Differential against a std::priority_queue oracle -----------------------

TEST(RadixHeap, MatchesAHeapOracleUnderMonotonePushes) {
    // Keys are generated the way Dijkstra generates them: current minimum plus
    // a non-negative increment. This keeps the monotone precondition satisfied
    // by construction rather than by assertion.
    for (std::uint64_t seed = 0; seed < 50; ++seed) {
        Rng            rng(seed);
        RadixHeap<Key> q;
        std::priority_queue<std::pair<Key, NodeId>, std::vector<std::pair<Key, NodeId>>,
                            std::greater<>>
            oracle;

        Key last = 0;
        for (std::uint32_t i = 0; i < 40; ++i) {
            const Key k = last + rng.next_below(100);
            q.push(i, k);
            oracle.emplace(k, i);
        }

        std::uint32_t next_node = 40;
        while (!oracle.empty()) {
            const auto expected = oracle.top();
            oracle.pop();
            const auto got = q.pop();

            ASSERT_EQ(got.key, expected.first) << "seed " << seed;
            last = got.key;

            // Occasionally push a new key above the one just extracted, which
            // is exactly what a relaxation does.
            if (rng.next_unit() < 0.3 && next_node < 500) {
                const Key k = last + rng.next_below(100);
                q.push(next_node, k);
                oracle.emplace(k, next_node);
                ++next_node;
            }
        }
        EXPECT_TRUE(q.empty()) << "seed " << seed;
    }
}

TEST(RadixHeap, RedistributionPreservesEveryEntry) {
    // Drives many redistribution passes by using a wide key range, then checks
    // that the multiset of popped keys matches what went in. A redistribution
    // bug would drop or duplicate entries.
    for (std::uint64_t seed = 0; seed < 30; ++seed) {
        Rng            rng(seed + 700);
        RadixHeap<Key> q;
        std::vector<Key> pushed;

        Key last = 0;
        for (std::uint32_t i = 0; i < 500; ++i) {
            const Key k = last + rng.next_below(100000);
            q.push(i, k);
            pushed.push_back(k);
        }
        std::vector<Key> popped;
        while (!q.empty()) {
            popped.push_back(q.pop().key);
        }
        std::sort(pushed.begin(), pushed.end());
        ASSERT_EQ(popped, pushed) << "seed " << seed;
        EXPECT_TRUE(std::is_sorted(popped.begin(), popped.end())) << "seed " << seed;
    }
}

// --- Compatibility reporting --------------------------------------------------

TEST(RadixHeapCompatibility, SupportsIntegerWeightsAndRejectsFloatingPoint) {
    // ESD.md section 7 requires incompatible configurations to be reported as
    // unsupported rather than silently producing invalid results.
    static_assert(lpq::radix_heap_supports<std::uint32_t>());
    static_assert(lpq::radix_heap_supports<std::uint64_t>());
    static_assert(!lpq::radix_heap_supports<double>());
    SUCCEED();
}

TEST(RadixHeapCompatibility, ExplainsWhyFloatingPointIsUnsupported) {
    // The reason string lands in result metadata, so a missing radix-heap row
    // in a results table is explained rather than merely absent.
    const std::string reason = lpq::radix_heap_unsupported_reason<double>();
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("unsigned integer"), std::string::npos);

    EXPECT_TRUE(std::string(lpq::radix_heap_unsupported_reason<std::uint32_t>()).empty())
        << "a supported configuration must carry no reason string";
}

}  // namespace
