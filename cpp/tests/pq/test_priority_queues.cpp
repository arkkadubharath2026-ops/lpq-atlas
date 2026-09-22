// =============================================================================
// Shared property tests for every priority queue (ESD.md section 24).
//
// Written as GoogleTest typed tests so that each queue is held to exactly the
// same contract. Adding a queue means adding one line to the type list below;
// it then inherits the full suite. That matters for a project whose central
// claim rests on comparing queues: if one implementation were tested more
// leniently than another, a difference in measured behaviour could reflect a
// difference in test coverage rather than in the algorithms.
//
// The invariant of ESD.md section 24 -- "pop() returns the minimum key" -- is
// asserted against an independently maintained reference multiset rather than
// against the queue's own view of itself.
//
// Note on ties: implementations legitimately differ in which of several
// equal-minimum nodes they return, since any of them is a valid extraction.
// These tests therefore assert on the *key* returned, and on the multiset of
// (node, key) pairs over a full drain, never on a specific node at a specific
// step.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/priority_queue.hpp"

namespace {

using lpq::NodeId;
using lpq::QueueEntry;
using lpq::Rng;

using Key = std::uint64_t;

// -----------------------------------------------------------------------------
// The queues under test
// -----------------------------------------------------------------------------
using QueueTypes = ::testing::Types<lpq::BinaryHeap<Key>,
                                    lpq::FourAryHeap<Key>,
                                    lpq::PairingHeap<Key>,
                                    lpq::FibonacciHeap<Key>>;

template <typename Q>
class PriorityQueueTest : public ::testing::Test {
protected:
    /// Builds a queue sized for `n` nodes.
    static Q make(std::size_t n) {
        Q q;
        q.reserve_nodes(n);
        return q;
    }
};

TYPED_TEST_SUITE(PriorityQueueTest, QueueTypes);

// --- Concept conformance ------------------------------------------------------

TYPED_TEST(PriorityQueueTest, SatisfiesThePriorityQueueConcept) {
    static_assert(lpq::PriorityQueueLike<TypeParam>);
    SUCCEED();
}

// --- Basic behaviour ----------------------------------------------------------

TYPED_TEST(PriorityQueueTest, StartsEmpty) {
    auto q = TestFixture::make(16);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);
}

TYPED_TEST(PriorityQueueTest, SingleElementRoundTrips) {
    auto q = TestFixture::make(16);
    q.push(3, 42);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1U);
    EXPECT_EQ(q.top().node, 3U);
    EXPECT_EQ(q.top().key, 42U);

    const auto e = q.pop();
    EXPECT_EQ(e.node, 3U);
    EXPECT_EQ(e.key, 42U);
    EXPECT_TRUE(q.empty());
}

TYPED_TEST(PriorityQueueTest, PopsInNonDecreasingKeyOrder) {
    // The defining invariant. Insertion order is deliberately adversarial:
    // descending, so a heap that merely preserved insertion order would fail.
    auto q = TestFixture::make(64);
    for (std::uint32_t i = 0; i < 50; ++i) {
        q.push(i, 100 - i);
    }
    Key previous = 0;
    for (std::uint32_t i = 0; i < 50; ++i) {
        const auto e = q.pop();
        EXPECT_GE(e.key, previous) << "keys must not decrease across pops";
        previous = e.key;
    }
    EXPECT_TRUE(q.empty());
}

TYPED_TEST(PriorityQueueTest, TopAgreesWithTheNextPop) {
    auto q = TestFixture::make(64);
    Rng  rng(7);
    for (std::uint32_t i = 0; i < 50; ++i) {
        q.push(i, rng.next_below(1000));
    }
    while (!q.empty()) {
        const auto peeked = q.top();
        const auto popped = q.pop();
        EXPECT_EQ(peeked.node, popped.node);
        EXPECT_EQ(peeked.key, popped.key);
    }
}

TYPED_TEST(PriorityQueueTest, HandlesDuplicateKeys) {
    // All keys equal: the hardest tie case. Every node must still come out
    // exactly once.
    auto q = TestFixture::make(32);
    for (std::uint32_t i = 0; i < 20; ++i) {
        q.push(i, 5);
    }
    std::set<NodeId> seen;
    while (!q.empty()) {
        const auto e = q.pop();
        EXPECT_EQ(e.key, 5U);
        EXPECT_TRUE(seen.insert(e.node).second) << "node " << e.node << " popped twice";
    }
    EXPECT_EQ(seen.size(), 20U);
}

TYPED_TEST(PriorityQueueTest, HandlesZeroAndMaximumKeys) {
    auto q = TestFixture::make(8);
    q.push(0, 0);
    q.push(1, std::numeric_limits<Key>::max());
    q.push(2, 1);
    EXPECT_EQ(q.pop().key, 0U);
    EXPECT_EQ(q.pop().key, 1U);
    EXPECT_EQ(q.pop().key, std::numeric_limits<Key>::max());
}

TYPED_TEST(PriorityQueueTest, ClearEmptiesAndAllowsReuse) {
    auto q = TestFixture::make(32);
    for (std::uint32_t i = 0; i < 20; ++i) {
        q.push(i, i);
    }
    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);

    // Reuse after clear must behave as a fresh queue. A stale position map
    // would make these pushes silently no-op as spurious decrease-keys.
    for (std::uint32_t i = 0; i < 20; ++i) {
        q.push(i, 100 - i);
    }
    EXPECT_EQ(q.size(), 20U);
    EXPECT_EQ(q.pop().key, 81U);
}

// --- Decrease-key -------------------------------------------------------------

TYPED_TEST(PriorityQueueTest, DecreaseKeyLowersAKeyAndReorders) {
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        auto q = TestFixture::make(32);
        for (std::uint32_t i = 0; i < 10; ++i) {
            q.push(i, 100 + i);
        }
        // Node 9 had the largest key; make it the smallest.
        q.decrease_key(9, 1);
        EXPECT_EQ(q.top().node, 9U);
        EXPECT_EQ(q.top().key, 1U);
        EXPECT_EQ(q.pop().node, 9U);
    }
}

TYPED_TEST(PriorityQueueTest, DecreaseKeyIgnoresANonImprovement) {
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        auto q = TestFixture::make(32);
        q.push(0, 10);
        q.decrease_key(0, 50);  // higher: must be ignored, not applied
        EXPECT_EQ(q.top().key, 10U);
        EXPECT_EQ(q.size(), 1U) << "a rejected decrease must not insert a duplicate";
    }
}

TYPED_TEST(PriorityQueueTest, DecreaseKeyOnAnAbsentNodeInserts) {
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        auto q = TestFixture::make(32);
        q.decrease_key(5, 7);
        EXPECT_EQ(q.size(), 1U);
        EXPECT_EQ(q.top().node, 5U);
    }
}

TYPED_TEST(PriorityQueueTest, ContainsTracksMembershipAcrossPops) {
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        auto q = TestFixture::make(32);
        EXPECT_FALSE(q.contains(3));
        q.push(3, 10);
        EXPECT_TRUE(q.contains(3));
        q.pop();
        EXPECT_FALSE(q.contains(3)) << "a popped node must no longer be contained";
    }
}

TYPED_TEST(PriorityQueueTest, PushingAnExistingNodeDoesNotDuplicateIt) {
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        auto q = TestFixture::make(32);
        q.push(4, 10);
        q.push(4, 3);
        EXPECT_EQ(q.size(), 1U);
        EXPECT_EQ(q.top().key, 3U);
        q.pop();
        EXPECT_TRUE(q.empty());
    }
}

TYPED_TEST(PriorityQueueTest, DecreaseKeyMaintainsOrderUnderRandomOperations) {
    // Mirrors the queue against a std::map oracle and checks the reported
    // minimum after every single operation, so a corruption is caught at the
    // step that caused it rather than at the end of a drain.
    if constexpr (TypeParam::kSupportsDecreaseKey) {
        constexpr std::uint32_t kNodes = 200;
        for (std::uint64_t seed = 0; seed < 40; ++seed) {
            Rng  rng(seed);
            auto q = TestFixture::make(kNodes);
            std::map<NodeId, Key> oracle;

            for (int step = 0; step < 2000; ++step) {
                const auto action = rng.next_below(10);
                if (action < 5 || oracle.empty()) {
                    const auto n = static_cast<NodeId>(rng.next_below(kNodes));
                    const Key  k = rng.next_below(10000);
                    q.push(n, k);
                    auto it = oracle.find(n);
                    if (it == oracle.end()) {
                        oracle[n] = k;
                    } else {
                        it->second = std::min(it->second, k);
                    }
                } else if (action < 8) {
                    const auto n = static_cast<NodeId>(rng.next_below(kNodes));
                    const Key  k = rng.next_below(10000);
                    q.decrease_key(n, k);
                    auto it = oracle.find(n);
                    if (it == oracle.end()) {
                        oracle[n] = k;
                    } else {
                        it->second = std::min(it->second, k);
                    }
                } else {
                    const auto e = q.pop();
                    auto       it = oracle.find(e.node);
                    ASSERT_NE(it, oracle.end())
                        << "seed " << seed << " step " << step << ": popped an absent node";
                    ASSERT_EQ(it->second, e.key) << "seed " << seed << " step " << step;
                    oracle.erase(it);
                }

                ASSERT_EQ(q.size(), oracle.size()) << "seed " << seed << " step " << step;
                if (!oracle.empty()) {
                    const Key expected_min =
                        std::min_element(oracle.begin(), oracle.end(),
                                         [](const auto& a, const auto& b) {
                                             return a.second < b.second;
                                         })->second;
                    ASSERT_EQ(q.top().key, expected_min)
                        << "seed " << seed << " step " << step;
                }
            }
        }
    }
}

// --- Drain equivalence --------------------------------------------------------

TYPED_TEST(PriorityQueueTest, FullDrainYieldsTheCorrectMultiset) {
    constexpr std::uint32_t kNodes = 500;
    for (std::uint64_t seed = 0; seed < 30; ++seed) {
        Rng  rng(seed + 1000);
        auto q = TestFixture::make(kNodes);
        std::map<NodeId, Key> oracle;

        for (std::uint32_t n = 0; n < kNodes; ++n) {
            const Key k = rng.next_below(100000);
            q.push(n, k);
            oracle[n] = k;
        }

        std::vector<Key> popped;
        popped.reserve(kNodes);
        while (!q.empty()) {
            const auto e = q.pop();
            auto       it = oracle.find(e.node);
            ASSERT_NE(it, oracle.end()) << "seed " << seed;
            ASSERT_EQ(it->second, e.key) << "seed " << seed;
            oracle.erase(it);
            popped.push_back(e.key);
        }
        EXPECT_TRUE(oracle.empty()) << "seed " << seed << ": not every node was popped";
        EXPECT_TRUE(std::is_sorted(popped.begin(), popped.end()))
            << "seed " << seed << ": drain was not in sorted order";
    }
}

TYPED_TEST(PriorityQueueTest, InterleavedPushAndPopStaysOrdered) {
    // A pattern closer to Dijkstra's than a bulk load/drain: the queue never
    // empties and pushes continue while pops happen.
    constexpr std::uint32_t kNodes = 300;
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        Rng  rng(seed + 2000);
        auto q = TestFixture::make(kNodes);
        std::map<NodeId, Key> oracle;

        for (int step = 0; step < 3000; ++step) {
            if (rng.next_unit() < 0.6) {
                const auto n = static_cast<NodeId>(rng.next_below(kNodes));
                const Key  k = rng.next_below(5000);
                q.push(n, k);
                auto it = oracle.find(n);
                if (it == oracle.end()) {
                    oracle[n] = k;
                } else {
                    it->second = std::min(it->second, k);
                }
            } else if (!oracle.empty()) {
                const auto e  = q.pop();
                auto       it = oracle.find(e.node);
                ASSERT_NE(it, oracle.end()) << "seed " << seed << " step " << step;
                ASSERT_EQ(it->second, e.key) << "seed " << seed << " step " << step;
                oracle.erase(it);
            }
        }
    }
}

// --- Instrumentation ----------------------------------------------------------

TYPED_TEST(PriorityQueueTest, CountsComparisons) {
#if LPQ_ENABLE_INSTRUMENTATION
    auto q = TestFixture::make(64);
    for (std::uint32_t i = 0; i < 50; ++i) {
        q.push(i, 50 - i);
    }
    EXPECT_GT(q.comparison_count(), 0U)
        << "a heap must perform comparisons to order 50 descending keys";
#else
    SUCCEED() << "instrumentation compiled out";
#endif
}

// --- Stress -------------------------------------------------------------------

TYPED_TEST(PriorityQueueTest, HandlesAscendingInsertionWithoutSiftWork) {
    // Best case for sift-up: each new key is the largest so far.
    auto q = TestFixture::make(1000);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        q.push(i, i);
    }
    for (std::uint32_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(q.pop().key, i);
    }
}

TYPED_TEST(PriorityQueueTest, HandlesDescendingInsertionWorstCase) {
    // Worst case for sift-up: every insert travels to the root.
    auto q = TestFixture::make(1000);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        q.push(i, 1000 - i);
    }
    Key previous = 0;
    while (!q.empty()) {
        const auto e = q.pop();
        EXPECT_GE(e.key, previous);
        previous = e.key;
    }
}

}  // namespace
