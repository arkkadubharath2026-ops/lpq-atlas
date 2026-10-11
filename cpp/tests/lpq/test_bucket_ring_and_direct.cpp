// =============================================================================
// Runtime-sized bucket ring, the hierarchical non-empty bitmap, the classical
// bucket queue, and the in-place (kDirect) learned extraction.
//
// What is established here, and against what:
//
//   1. BucketBitmap against a std::set oracle, including every word and level
//      boundary, the empty state and queries past the end.
//   2. BucketSubstrate over a grid of ring sizes and bucket widths against a
//      std::priority_queue / std::multiset oracle, with keys placed ON the
//      window boundary (base + ring * width - 1, + 0, + 1), far beyond it, and
//      separated by gaps that force redistribution.
//   3. ClassicalBucketQueue and LearnedPriorityQueue(kDirect) driven through the
//      templated dijkstra() against reference_dijkstra_heap on random and
//      structured graphs, in single-source and single-target mode.
//   4. A* with a consistent heuristic on grids through the same queues.
//
// Seeds. Every Rng below is constructed from a literal or from a literal plus
// loop indices, and every failure message names the seed. Any seed that ever
// produces a failure is promoted to the "Regression" section at the end of this
// file as a permanently named test (CLAUDE.md, "Randomised testing").
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lpq/algorithms/astar.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/heuristics.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/lpq/bucket_substrate.hpp"
#include "lpq/lpq/classical_bucket_queue.hpp"
#include "lpq/lpq/extraction_observer.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/lpq/pooled_bucket_queue.hpp"
#include "lpq/lpq/queue_policy.hpp"
#include "lpq/pq/priority_queue.hpp"
#include "lpq_test/differential.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight    = std::uint32_t;
using Substrate = BucketSubstrate<Weight>;
using Key       = Substrate::KeyType;
using Graph     = CsrGraph<Weight>;
using Classical = ClassicalBucketQueue<Weight>;

static_assert(PriorityQueueLike<Classical>);
static_assert(LazyDeletionQueue<Classical>);

constexpr std::size_t kNpos = BucketBitmap::npos;

// =============================================================================
// 1. BucketBitmap
// =============================================================================

constexpr std::size_t kBitmapSizes[] = {1,    63,    64,     65,      4095,
                                        4096, 4097, 262144, 1U << 20};

void expect_find_next_matches(const BucketBitmap& bm, const std::set<std::size_t>& oracle,
                              std::size_t size, std::size_t from) {
    std::size_t expected = kNpos;
    if (from < size) {
        const auto it = oracle.lower_bound(from);
        if (it != oracle.end()) {
            expected = *it;
        }
    }
    ASSERT_EQ(bm.find_next(from), expected) << "find_next(" << from << ") with size " << size;
}

// Walks the whole set through find_next and compares it, in order, to the oracle.
void expect_chain_matches(const BucketBitmap& bm, const std::set<std::size_t>& oracle) {
    auto it = oracle.begin();
    for (std::size_t i = bm.find_next(0); i != kNpos; i = bm.find_next(i + 1)) {
        ASSERT_NE(it, oracle.end()) << "bitmap reports " << i << " but the oracle is exhausted";
        ASSERT_EQ(i, *it);
        ++it;
    }
    ASSERT_EQ(it, oracle.end()) << "bitmap ended before the oracle; next oracle member " << *it;
}

// Every query position, for sizes small enough to afford it.
void expect_exhaustive_find_next(const BucketBitmap& bm, const std::set<std::size_t>& oracle,
                                 std::size_t size) {
    for (std::size_t j = 0; j < size + 130; ++j) {
        ASSERT_NO_FATAL_FAILURE(expect_find_next_matches(bm, oracle, size, j));
    }
    ASSERT_EQ(bm.find_next(kNpos), kNpos);
}

TEST(BucketBitmap, StartsEmptyAtEverySize) {
    for (std::size_t size : kBitmapSizes) {
        SCOPED_TRACE("size " + std::to_string(size));
        BucketBitmap bm(size);
        EXPECT_EQ(bm.find_next(0), kNpos);
        EXPECT_EQ(bm.find_next(size - 1), kNpos);
        EXPECT_EQ(bm.find_next(size), kNpos);
        EXPECT_EQ(bm.find_next(size + 1), kNpos);
        EXPECT_EQ(bm.find_next(size + 100000), kNpos);
        EXPECT_EQ(bm.find_next(kNpos), kNpos);
        EXPECT_FALSE(bm.test(0));
        EXPECT_FALSE(bm.test(size - 1));
        EXPECT_GT(bm.memory_bytes(), 0U);
    }
}

TEST(BucketBitmap, AZeroBitBitmapIsConstructibleAndAlwaysEmpty) {
    BucketBitmap bm(0);
    EXPECT_EQ(bm.find_next(0), kNpos);
    EXPECT_EQ(bm.find_next(63), kNpos);
    EXPECT_EQ(bm.find_next(kNpos), kNpos);
    EXPECT_NO_THROW(bm.clear_all());
}

TEST(BucketBitmap, ADefaultConstructedBitmapMustBeResetBeforeUse) {
    BucketBitmap bm;
    bm.reset(100);
    bm.set(99);
    EXPECT_EQ(bm.find_next(0), 99U);
    EXPECT_TRUE(bm.test(99));
}

TEST(BucketBitmap, ResetDiscardsPriorContentAndResizes) {
    BucketBitmap bm(4097);
    bm.set(0);
    bm.set(4096);
    bm.reset(65);
    EXPECT_EQ(bm.find_next(0), kNpos);
    bm.set(64);
    EXPECT_EQ(bm.find_next(0), 64U);
    EXPECT_EQ(bm.find_next(65), kNpos);
}

// One member at a time, at every word and level boundary: the member must be
// found from any earlier position, not from any later one, and clearing it must
// clear the summary levels too (otherwise find_next(0) would still descend).
TEST(BucketBitmap, SingleMemberAtBoundariesIsFoundAndFullyCleared) {
    for (std::size_t size : kBitmapSizes) {
        std::set<std::size_t> positions{0, size - 1};
        if (size >= 2) {
            positions.insert(1);
            positions.insert(size - 2);
        }
        for (std::size_t w : {1U, 2U, 4U, 63U, 64U, 65U, 4095U, 4096U, 4097U, 16383U, 16384U}) {
            for (std::size_t d : {0U, 1U}) {
                const std::size_t base = w * 64U;
                if (base >= d && base - d < size) {
                    positions.insert(base - d);
                }
                if (base + d < size) {
                    positions.insert(base + d);
                }
            }
        }
        for (std::size_t p : {62U, 63U, 64U, 65U, 127U, 128U, 4094U, 4095U, 4096U, 4097U,
                              262143U, 262144U, 262145U}) {
            if (p < size) {
                positions.insert(p);
            }
        }

        BucketBitmap bm(size);
        for (std::size_t p : positions) {
            SCOPED_TRACE("size " + std::to_string(size) + " member " + std::to_string(p));
            bm.set(p);
            ASSERT_TRUE(bm.test(p));
            ASSERT_EQ(bm.find_next(0), p);
            ASSERT_EQ(bm.find_next(p), p);
            if (p > 0) {
                ASSERT_EQ(bm.find_next(p - 1), p);
            }
            ASSERT_EQ(bm.find_next(p + 1), kNpos);
            ASSERT_EQ(bm.find_next(size), kNpos);
            bm.clear(p);
            ASSERT_FALSE(bm.test(p));
            ASSERT_EQ(bm.find_next(0), kNpos) << "summary level kept a stale bit";
        }
    }
}

// Exhaustive for the sizes small enough that every pair can be tried: set two
// members, clear them in both orders, and compare every query to the oracle.
TEST(BucketBitmap, ExhaustivePairsOnTheSmallSizes) {
    for (std::size_t size : {1U, 63U, 64U, 65U}) {
        for (std::size_t p = 0; p < size; ++p) {
            for (std::size_t q = p; q < size; ++q) {
                for (int order = 0; order < 2; ++order) {
                    SCOPED_TRACE("size " + std::to_string(size) + " pair " + std::to_string(p) +
                                 "," + std::to_string(q) + " order " + std::to_string(order));
                    BucketBitmap          bm(size);
                    std::set<std::size_t> oracle;
                    bm.set(p);
                    bm.set(q);
                    oracle.insert(p);
                    oracle.insert(q);
                    ASSERT_NO_FATAL_FAILURE(expect_chain_matches(bm, oracle));
                    ASSERT_NO_FATAL_FAILURE(expect_exhaustive_find_next(bm, oracle, size));
                    bm.clear(order == 0 ? p : q);
                    oracle.erase(order == 0 ? p : q);
                    if (p == q) {
                        oracle.clear();
                    }
                    ASSERT_NO_FATAL_FAILURE(expect_chain_matches(bm, oracle));
                    ASSERT_NO_FATAL_FAILURE(expect_exhaustive_find_next(bm, oracle, size));
                    bm.clear(order == 0 ? q : p);
                    oracle.clear();
                    ASSERT_NO_FATAL_FAILURE(expect_exhaustive_find_next(bm, oracle, size));
                }
            }
        }
    }
}

TEST(BucketBitmap, SetAndClearAreIdempotent) {
    BucketBitmap bm(4097);
    bm.set(100);
    bm.set(100);
    bm.set(4096);
    bm.clear(100);
    bm.clear(100);  // already clear: must not disturb the summary of its neighbours
    EXPECT_EQ(bm.find_next(0), 4096U);
    bm.clear(4096);
    bm.clear(4096);
    EXPECT_EQ(bm.find_next(0), kNpos);
}

TEST(BucketBitmap, ClearAllEmptiesEveryLevelAndAllowsReuse) {
    for (std::size_t size : kBitmapSizes) {
        BucketBitmap bm(size);
        for (std::size_t i = 0; i < size; i += 61) {
            bm.set(i);
        }
        bm.set(size - 1);
        bm.clear_all();
        EXPECT_EQ(bm.find_next(0), kNpos) << "size " << size;
        bm.set(size - 1);
        EXPECT_EQ(bm.find_next(0), size - 1) << "size " << size;
    }
}

// The randomised comparison. Indices are drawn from a mixture that
// concentrates on the places a hierarchical bitmap goes wrong: word edges, the
// edges of the second-level words (multiples of 4096), the ends, and the
// neighbourhood of existing members. Dense and sparse phases alternate so the
// summary bits are both set and cleared in bulk.
void bitmap_trial(std::size_t size, std::uint64_t seed, int ops) {
    SCOPED_TRACE("size " + std::to_string(size) + " seed " + std::to_string(seed));
    Rng                   rng(seed);
    BucketBitmap          bm(size);
    std::set<std::size_t> oracle;

    auto pick = [&]() -> std::size_t {
        switch (rng.next_below(6)) {
            case 0:
                return static_cast<std::size_t>(rng.next_below(size));
            case 1: {
                const std::size_t words = (size + 63U) / 64U;
                const std::size_t w     = static_cast<std::size_t>(rng.next_below(words));
                static constexpr std::size_t kOff[] = {0, 1, 62, 63};
                return std::min(size - 1, w * 64U + kOff[rng.next_below(4)]);
            }
            case 2: {
                static constexpr std::size_t kEnds[] = {0, 1, 2};
                const std::size_t e = kEnds[rng.next_below(3)];
                return rng.next_below(2) == 0 ? std::min(e, size - 1)
                                              : (size - 1 >= e ? size - 1 - e : 0);
            }
            case 3: {
                const std::size_t blocks = (size + 4095U) / 4096U;
                const std::size_t b      = static_cast<std::size_t>(rng.next_below(blocks));
                static constexpr std::size_t kOff[] = {0, 1, 63, 64, 4094, 4095};
                return std::min(size - 1, b * 4096U + kOff[rng.next_below(6)]);
            }
            case 4:
                if (!oracle.empty()) {
                    auto it = oracle.begin();
                    std::advance(it, static_cast<std::ptrdiff_t>(rng.next_below(oracle.size())));
                    const std::size_t m = *it;
                    const std::size_t d = static_cast<std::size_t>(rng.next_below(3));
                    return rng.next_below(2) == 0 ? std::min(size - 1, m + d) : (m >= d ? m - d : 0);
                }
                return static_cast<std::size_t>(rng.next_below(size));
            default:
                return static_cast<std::size_t>(rng.next_below(std::min<std::size_t>(size, 70)));
        }
    };

    const int phase_len = std::max(1, ops / 8);
    for (int op = 0; op < ops; ++op) {
        // Phases: fill (mostly sets), drain (mostly clears), mixed.
        const int phase  = (op / phase_len) % 3;
        const int p_set  = phase == 0 ? 90 : (phase == 1 ? 10 : 50);
        const int choice = static_cast<int>(rng.next_below(100));

        if (choice < p_set) {
            const std::size_t i = pick();
            bm.set(i);
            oracle.insert(i);
            ASSERT_TRUE(bm.test(i));
        } else if (choice < p_set + (100 - p_set) / 2) {
            const std::size_t i = pick();
            bm.clear(i);
            oracle.erase(i);
            ASSERT_FALSE(bm.test(i));
        } else {
            const std::size_t i = pick();
            ASSERT_EQ(bm.test(i), oracle.count(i) != 0) << "test(" << i << ")";
        }

        // find_next from a mix of positions, including past the end.
        std::size_t from;
        switch (rng.next_below(4)) {
            case 0: from = pick(); break;
            case 1: from = static_cast<std::size_t>(rng.next_below(size + 130)); break;
            case 2: from = size + static_cast<std::size_t>(rng.next_below(200)); break;
            default: from = kNpos; break;
        }
        ASSERT_NO_FATAL_FAILURE(expect_find_next_matches(bm, oracle, size, from));

        if (op % phase_len == phase_len - 1) {
            ASSERT_NO_FATAL_FAILURE(expect_chain_matches(bm, oracle));
            if (size <= 4097) {
                ASSERT_NO_FATAL_FAILURE(expect_exhaustive_find_next(bm, oracle, size));
            }
        }
    }
    ASSERT_NO_FATAL_FAILURE(expect_chain_matches(bm, oracle));

    bm.clear_all();
    oracle.clear();
    ASSERT_NO_FATAL_FAILURE(expect_chain_matches(bm, oracle));
}

TEST(BucketBitmap, RandomisedAgainstAStdSetOracle) {
    std::uint64_t seed = 20261011;
    for (std::size_t size : kBitmapSizes) {
        const int ops = size <= 4097 ? 20000 : 60000;
        for (int rep = 0; rep < 3; ++rep) {
            ASSERT_NO_FATAL_FAILURE(bitmap_trial(size, seed++, ops));
        }
    }
}

// Fully dense then fully drained: the summary levels must follow in both
// directions, including the partially-filled last word of each level.
TEST(BucketBitmap, FillCompletelyThenDrainInRandomOrder) {
    for (std::size_t size : kBitmapSizes) {
        SCOPED_TRACE("size " + std::to_string(size));
        Rng                      rng(31337U + size);
        BucketBitmap             bm(size);
        std::vector<std::size_t> order(size);
        for (std::size_t i = 0; i < size; ++i) {
            order[i] = i;
        }
        for (std::size_t i = size; i > 1; --i) {
            std::swap(order[i - 1], order[static_cast<std::size_t>(rng.next_below(i))]);
        }
        for (std::size_t i = 0; i < size; ++i) {
            bm.set(i);
        }
        for (std::size_t j = 0; j < size; j += 1 + size / 997) {
            ASSERT_EQ(bm.find_next(j), j);
        }
        std::set<std::size_t> oracle;
        for (std::size_t i = 0; i < size; ++i) {
            oracle.insert(i);
        }
        std::size_t step = 0;
        for (std::size_t idx : order) {
            bm.clear(idx);
            oracle.erase(idx);
            if (++step % (1 + size / 64) == 0) {
                ASSERT_NO_FATAL_FAILURE(expect_find_next_matches(bm, oracle, size, idx));
                ASSERT_NO_FATAL_FAILURE(expect_find_next_matches(bm, oracle, size, 0));
            }
        }
        EXPECT_EQ(bm.find_next(0), kNpos);
    }
}

// =============================================================================
// 2. BucketSubstrate: ring sizes x widths against an oracle
// =============================================================================

constexpr std::size_t kRings[]  = {2, 4, 64, 1024, 65536};
constexpr Key         kWidths[] = {1, 2, 3, 37, 1000};

TEST(BucketRing, RejectsInvalidRingSizes) {
    for (std::size_t bad : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{6},
                            std::size_t{100}, std::size_t{1000}, std::size_t{1023},
                            std::size_t{1025}, std::size_t{1} << 23,
                            (std::size_t{1} << 22) + (std::size_t{1} << 21),
                            std::size_t{1} << 40}) {
        EXPECT_THROW(Substrate(1, bad), InvalidConfigError) << "ring " << bad;
        EXPECT_THROW(Classical(1, bad), InvalidConfigError) << "ring " << bad;
    }
    EXPECT_THROW(Substrate(0, 64), InvalidConfigError) << "zero width";
}

TEST(BucketRing, ReportsTheRequestedRingAndDefaultsToTheOldConstant) {
    for (std::size_t ring : kRings) {
        EXPECT_EQ(Substrate(1, ring).ring_size(), ring);
    }
    EXPECT_EQ(Substrate(5).ring_size(), Substrate::kBucketCount);
    EXPECT_EQ(Substrate().ring_size(), Substrate::kBucketCount);
}

// The oracle for one substrate. Ordering of KEYS is checked against a
// std::priority_queue; the multiset of (key, node) pairs is checked against a
// std::multiset, because ties may legitimately be broken either way.
class MonotoneHarness {
public:
    MonotoneHarness(Substrate& q, Key width, std::size_t ring) : q_(q), width_(width), ring_(ring) {}

    void reset() {
        items_.clear();
        keys_  = {};
        floor_ = 0;
    }

    [[nodiscard]] bool   empty() const { return items_.empty(); }

    void push(NodeId node, Key key) {
        q_.push(node, key);
        items_.emplace(key, node);
        keys_.push(key);
    }

    void check_state() {
        ASSERT_EQ(q_.size(), items_.size());
        ASSERT_EQ(q_.empty(), items_.empty());
    }

    // top() is const and O(ring); sampled by the caller.
    void check_top() {
        if (items_.empty()) {
            return;
        }
        const auto t = q_.top();
        ASSERT_EQ(t.key, keys_.top());
        ASSERT_GE(items_.count({t.key, t.node}), 1U) << "top() returned a (key,node) not queued";
    }

    void expect_popped(Key key, NodeId node) {
        ASSERT_FALSE(keys_.empty()) << "substrate popped from an empty oracle";
        ASSERT_EQ(key, keys_.top()) << "pop is not in key order";
        keys_.pop();
        const auto it = items_.find({key, node});
        ASSERT_NE(it, items_.end()) << "popped (node " << node << ", key " << key
                                    << ") which is not queued";
        items_.erase(it);
        floor_ = key;
    }

    // One extraction through a randomly chosen protocol. The protocols are the
    // substrate's whole extraction surface: pop(), and the in-place accessors
    // used by LearnedPriorityQueue.
    void pop_one(Rng& rng) {
        ASSERT_FALSE(items_.empty());
        const Key want = keys_.top();

        switch (rng.next_below(5)) {
            case 0: {
                const auto e = q_.pop();
                ASSERT_NO_FATAL_FAILURE(expect_popped(e.key, e.node));
                return;
            }
            case 1:
            case 2:
            case 3:
            case 4: {
                const std::uint64_t proto = rng.next_below(4);
                const Key           bound = q_.current_bound();
                const auto          span  = q_.peek_current_bucket();
                ASSERT_FALSE(span.empty());
                ASSERT_EQ(span.size(), q_.current_bucket_size());
                // The bound is a lower bound on everything queued.
                ASSERT_LE(bound, want) << "current_bound() above the true minimum";

                std::size_t arg = 0;
                for (std::size_t i = 0; i < span.size(); ++i) {
                    ASSERT_GE(span[i].key, bound) << "entry below the cached bound";
                    ASSERT_GE(span[i].key, q_.base());
                    ASSERT_LT(span[i].key, q_.base() + width_) << "entry outside its bucket";
                    if (span[i].key < span[arg].key) {
                        arg = i;
                    }
                }
                ASSERT_EQ(span[arg].key, want) << "the current bucket does not hold the minimum";

                if (proto == 0) {  // peek + take_from_current
                    const Key    k0 = span[arg].key;
                    const NodeId n0 = span[arg].node;
                    const auto   e  = q_.take_from_current(arg);
                    ASSERT_EQ(e.key, k0);
                    ASSERT_EQ(e.node, n0);
                    ASSERT_NO_FATAL_FAILURE(expect_popped(e.key, e.node));
                } else if (proto == 1) {  // peek + remove_from_current_bucket
                    const NodeId node = span[arg].node;
                    // An absent entry is reported, not removed.
                    ASSERT_FALSE(q_.remove_from_current_bucket(node, want + width_ * 4 + 1));
                    ASSERT_EQ(q_.size(), items_.size());
                    ASSERT_TRUE(q_.remove_from_current_bucket(node, want));
                    ASSERT_NO_FATAL_FAILURE(expect_popped(want, node));
                } else if (proto == 2) {  // rescan_current_bucket + take
                    const std::size_t idx = q_.rescan_current_bucket();
                    const auto        sp2 = q_.peek_current_bucket();
                    ASSERT_LT(idx, sp2.size());
                    ASSERT_EQ(sp2[idx].key, want);
                    // A full scan tightens the cached bound to the exact minimum.
                    ASSERT_EQ(q_.current_bound(), want);
                    const NodeId node = sp2[idx].node;
                    const auto   e    = q_.take_from_current(idx);
                    ASSERT_EQ(e.node, node);
                    ASSERT_NO_FATAL_FAILURE(expect_popped(e.key, e.node));
                } else {  // caller scanned the whole bucket: set_current_bound + take
                    q_.set_current_bound(want);
                    ASSERT_EQ(q_.current_bound(), want);
                    const std::uint64_t before = q_.comparison_count();
                    q_.add_comparisons(7);
                    ASSERT_EQ(q_.comparison_count(), before + 7);
                    const auto e = q_.take_from_current(arg);
                    ASSERT_NO_FATAL_FAILURE(expect_popped(e.key, e.node));
                }
                return;
            }
            default:
                FAIL() << "unreachable";
        }
    }

    // A key to push. Biased toward the places the ring logic is delicate, and
    // always >= the last extracted key, as a monotone consumer would push.
    [[nodiscard]] Key draw_key(Rng& rng, bool allow_huge) const {
        const Key span = static_cast<Key>(ring_) * width_;
        const Key base = q_.base();
        const Key fl   = floor_;
        auto below = [&](Key n) { return n == 0 ? Key{0} : static_cast<Key>(rng.next_below(n)); };
        switch (rng.next_below(13)) {
            case 0: return fl;                       // tie with the last extraction
            case 1: return fl + below(width_);       // same bucket as the last extraction
            case 2: return fl + below(3 * width_);
            case 3: return fl + below(span);         // anywhere in the ring
            case 4: return base + span - 1;          // last ring slot
            case 5: return base + span;              // first overflow key
            case 6: return base + span + 1;
            case 7: return base + span + below(width_);
            case 8: return fl + below(2 * span);
            case 9: return fl + below(span * 8);
            case 10: return fl + (Key{1} << 20) + below(Key{1} << 20);  // wide gap
            case 11:
                return allow_huge && fl < (Key{1} << 45) ? fl + (Key{1} << 40) + below(Key{1} << 30)
                                                         : fl + below(width_);
            default: return base + span * 2 - 1;
        }
    }

private:
    Substrate&  q_;
    Key         width_;
    std::size_t ring_;
    Key         floor_ = 0;
    std::multiset<std::pair<Key, NodeId>>                       items_;
    std::priority_queue<Key, std::vector<Key>, std::greater<Key>> keys_;
};

// One self-contained trial: a fresh queue, so that (ring, width, seed) alone
// replays it exactly. The trial runs two segments on the SAME queue, separated
// by either a full drain (reuse without clear(), the base left wherever the last
// extraction put it) or a clear() with entries still in the ring and in overflow.
void substrate_trial(Key width, std::size_t ring, std::uint64_t seed, int ops_per_segment = 750) {
    SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width) + " seed " +
                 std::to_string(seed));
    Rng             rng(seed);
    Substrate       q(width, ring);
    MonotoneHarness h(q, width, ring);

    const std::uint64_t node_ranges[] = {1, 4, 64, 100000};
    const NodeId        node_range    = static_cast<NodeId>(node_ranges[rng.next_below(4)]);
    const bool          allow_huge    = rng.next_below(2) == 0;
    const std::size_t   top_every     = ring / 256 + 1;

    for (int segment = 0; segment < 2; ++segment) {
        for (int op = 0; op < ops_per_segment; ++op) {
            // Phase mix: push-heavy, balanced, pop-heavy.
            const int  phase  = (op / 150) % 3;
            const int  p_push = phase == 0 ? 80 : (phase == 1 ? 50 : 25);
            const bool push   = h.empty() || static_cast<int>(rng.next_below(100)) < p_push;

            if (push) {
                const NodeId node = static_cast<NodeId>(rng.next_below(node_range));
                const Key    key  = h.draw_key(rng, allow_huge);
                h.push(node, key);
            } else {
                ASSERT_NO_FATAL_FAILURE(h.pop_one(rng)) << "segment " << segment << " op " << op;
            }
            ASSERT_NO_FATAL_FAILURE(h.check_state());
            if (static_cast<std::size_t>(op) % top_every == 0) {
                ASSERT_NO_FATAL_FAILURE(h.check_top());
            }

            // A key under the base is refused, and the refusal leaves the queue as it was.
            if (q.base() > 0 && rng.next_below(40) == 0) {
                const std::size_t before = q.size();
                EXPECT_THROW(q.push(1, q.base() - 1), InternalError);
                ASSERT_EQ(q.size(), before);
            }
        }

        if (segment == 1 || rng.next_below(2) == 0) {
            while (!h.empty()) {
                ASSERT_NO_FATAL_FAILURE(h.pop_one(rng));
            }
            ASSERT_TRUE(q.empty());
            ASSERT_THROW((void)q.pop(), InternalError);
        } else {
            q.clear();
            ASSERT_TRUE(q.empty());
            ASSERT_EQ(q.size(), 0U);
            ASSERT_EQ(q.base(), 0U);
            h.reset();
        }
        ASSERT_NO_FATAL_FAILURE(h.check_state());
    }
}

TEST(BucketRing, RandomisedMonotoneWorkloadsMatchAnOracleAcrossRingsAndWidths) {
    std::uint64_t seed = 7100000;
    for (std::size_t ring : kRings) {
        for (Key width : kWidths) {
            for (int trial = 0; trial < 14; ++trial) {
                ASSERT_NO_FATAL_FAILURE(substrate_trial(width, ring, seed++));
            }
        }
    }
}

// Keys at base + ring * width - 1 / + 0 / + 1, in EVERY insertion order, with
// distinct node ids and with one node id throughout (duplicate node, different
// keys: lazy deletion's normal case).
TEST(BucketRing, KeysStraddlingTheWindowEdgeComeOutSortedInEveryInsertionOrder) {
    for (std::size_t ring : kRings) {
        for (Key width : kWidths) {
            const Key S = static_cast<Key>(ring) * width;
            const std::vector<Key> keys{0, S - 1, S, S + 1, 3 * S + 5};
            for (int same_node = 0; same_node < 2; ++same_node) {
                std::vector<std::size_t> perm(keys.size());
                for (std::size_t i = 0; i < perm.size(); ++i) {
                    perm[i] = i;
                }
                do {
                    Substrate q(width, ring);
                    for (std::size_t i : perm) {
                        q.push(same_node != 0 ? 7U : static_cast<NodeId>(i), keys[i]);
                    }
                    std::string order;
                    for (std::size_t i : perm) {
                        order += std::to_string(i);
                    }
                    SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width) +
                                 " order " + order + " same_node " + std::to_string(same_node));
                    ASSERT_EQ(q.top().key, 0U);
                    for (Key expected : keys) {
                        const auto e = q.pop();
                        ASSERT_EQ(e.key, expected);
                    }
                    ASSERT_TRUE(q.empty());
                } while (std::next_permutation(perm.begin(), perm.end()));
            }
        }
    }
}

// The same edge, but measured from a base that has ADVANCED: the window slides,
// so "base + ring * width - 1" lands in a different slot than it did at base 0.
TEST(BucketRing, KeysStraddlingTheWindowEdgeAfterTheBaseHasAdvanced) {
    for (std::size_t ring : kRings) {
        for (Key width : kWidths) {
            Substrate q(width, ring);
            SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width));
            const Key start = width * 5 + 2;
            q.push(1, start);
            ASSERT_EQ(q.pop().key, start);
            const Key edge = q.base() + static_cast<Key>(ring) * width;
            std::vector<Key> keys{start, edge - 1, edge, edge + 1, edge + width, edge * 2};
            std::vector<std::size_t> perm{0, 1, 2, 3, 4, 5};
            do {
                for (std::size_t i : perm) {
                    q.push(static_cast<NodeId>(i), keys[i]);
                }
                for (Key expected : keys) {
                    ASSERT_EQ(q.pop().key, expected);
                }
                ASSERT_TRUE(q.empty());
                // The queue is empty, base may sit anywhere <= keys.back(); the
                // next permutation re-pushes from `start`, which must still be
                // legal, so clear() to restore the base.
                q.clear();
                q.push(1, start);
                (void)q.pop();
            } while (std::next_permutation(perm.begin(), perm.end()));
        }
    }
}

TEST(BucketRing, HugeGapsBetweenKeysForceRedistributionAndStayOrdered) {
    for (std::size_t ring : kRings) {
        for (Key width : kWidths) {
            SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width));
            Substrate q(width, ring);
            const std::vector<Key> keys{0, 1, (Key{1} << 20), (Key{1} << 20) + 1, Key{1} << 33,
                                        (Key{1} << 33) + width, Key{1} << 40,
                                        (Key{1} << 40) + 12345, Key{1} << 50};
            // Reverse insertion: everything beyond the first goes to overflow.
            for (std::size_t i = keys.size(); i-- > 0;) {
                q.push(static_cast<NodeId>(i), keys[i]);
            }
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto e = q.pop();
                ASSERT_EQ(e.key, keys[i]);
                ASSERT_EQ(e.node, i);
                // After each extraction push something at the current floor: a
                // monotone consumer would, and it exercises re-bucketing right
                // after a redistribute moved the base a long way.
                if (i + 1 < keys.size() && i % 2 == 0) {
                    q.push(900 + static_cast<NodeId>(i), keys[i]);
                    const auto again = q.pop();
                    ASSERT_EQ(again.key, keys[i]);
                    ASSERT_EQ(again.node, 900 + i);
                }
            }
            ASSERT_TRUE(q.empty());
        }
    }
}

TEST(BucketRing, ZeroKeysAndDuplicateNodesWithDifferentKeys) {
    for (std::size_t ring : kRings) {
        for (Key width : kWidths) {
            SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width));
            Substrate q(width, ring);
            for (int i = 0; i < 5; ++i) {
                q.push(3, 0);
            }
            q.push(3, 1);
            q.push(3, width);
            q.push(3, static_cast<Key>(ring) * width + 9);
            q.push(3, 0);
            ASSERT_EQ(q.size(), 9U);
            for (int i = 0; i < 6; ++i) {
                const auto e = q.pop();
                ASSERT_EQ(e.key, 0U);
                ASSERT_EQ(e.node, 3U);
            }
            ASSERT_EQ(q.pop().key, 1U);
            ASSERT_EQ(q.pop().key, width);
            ASSERT_EQ(q.pop().key, static_cast<Key>(ring) * width + 9);
            ASSERT_TRUE(q.empty());
        }
    }
}

// clear() reuse across many cycles, including clears that leave the ring and
// the overflow list populated, and rings large enough that visiting every
// bucket would be the slow (and, with the bitmap, avoided) path.
TEST(BucketRing, ClearReuseAcrossManyCycles) {
    std::uint64_t seed = 8200000;
    for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{65536}}) {
        for (Key width : {Key{1}, Key{37}}) {
            Substrate q(width, ring);
            Rng       rng(seed++);
            SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width) +
                         " seed " + std::to_string(seed - 1));
            const Key span = static_cast<Key>(ring) * width;
            for (int cycle = 0; cycle < 1500; ++cycle) {
                std::multiset<std::pair<Key, NodeId>> items;
                const int n = static_cast<int>(rng.next_below(30));
                for (int i = 0; i < n; ++i) {
                    Key key;
                    switch (rng.next_below(4)) {
                        case 0: key = rng.next_below(width * 3 + 1); break;
                        case 1: key = rng.next_below(span + 1); break;
                        case 2: key = span + rng.next_below(span * 3 + 1); break;
                        default: key = Key{1} << (20 + rng.next_below(20)); break;
                    }
                    const NodeId node = static_cast<NodeId>(rng.next_below(50));
                    q.push(node, key);
                    items.emplace(key, node);
                }
                // Pop a random prefix so the base has moved before the clear.
                const int pops = n == 0 ? 0 : static_cast<int>(rng.next_below(n + 1));
                for (int i = 0; i < pops; ++i) {
                    const auto e = q.pop();
                    ASSERT_EQ(e.key, items.begin()->first) << "cycle " << cycle;
                    const auto it = items.find({e.key, e.node});
                    ASSERT_NE(it, items.end()) << "cycle " << cycle;
                    items.erase(it);
                }
                ASSERT_EQ(q.size(), items.size());
                q.clear();
                ASSERT_TRUE(q.empty()) << "cycle " << cycle;
                ASSERT_EQ(q.size(), 0U);
                ASSERT_EQ(q.base(), 0U);
                ASSERT_THROW((void)q.top(), InternalError);
                // No residue: a fresh push of key 0 is accepted and is the only entry.
                if (cycle % 7 == 0) {
                    q.push(77, 0);
                    const auto e = q.pop();
                    ASSERT_EQ(e.node, 77U);
                    ASSERT_EQ(e.key, 0U);
                    ASSERT_TRUE(q.empty());
                    ASSERT_THROW((void)q.pop(), InternalError);
                }
            }
        }
    }
}

// An overflow entry must keep its model score when it is re-filed, whether by
// the window sliding over it or by a redistribute.
TEST(BucketRing, OverflowEntriesKeepTheirScoreWhenRefiled) {
    {   // The window slides over the parked entry (absorb_overflow).
        Substrate q(1, 4);
        q.set_front_size(4);
        q.push(10, 0, 0.25F);
        q.push(11, 5, 0.75F);  // offset 5 >= ring 4: parked in overflow
        q.push(12, 3, 0.5F);
        ASSERT_EQ(q.pop().key, 0U);
        ASSERT_EQ(q.pop().key, 3U);
        const auto bucket = q.current_bucket();
        ASSERT_EQ(bucket.size(), 1U);
        EXPECT_EQ(bucket[0].node, 11U);
        EXPECT_EQ(bucket[0].key, 5U);
        EXPECT_EQ(bucket[0].score, 0.75F);
    }
    {   // The ring empties, so everything left is redistributed.
        Substrate q(1, 2);
        q.set_front_size(4);
        q.push(1, 0, 0.1F);
        q.push(2, 1000, 0.9F);
        ASSERT_EQ(q.pop().key, 0U);
        const auto bucket = q.current_bucket();
        ASSERT_EQ(bucket.size(), 1U);
        EXPECT_EQ(bucket[0].key, 1000U);
        EXPECT_EQ(bucket[0].score, 0.9F);
        EXPECT_EQ(q.base(), 1000U);
    }
}

// With only pushes into one bucket, the front is exactly the k best scores,
// highest first -- whatever the ring size.
TEST(BucketRing, FrontIsTheBestScoresForAPurePushWorkload) {
    std::uint64_t seed = 9300000;
    for (std::size_t ring : kRings) {
        for (std::size_t k : {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{32}}) {
            Rng rng(seed++);
            SCOPED_TRACE("ring " + std::to_string(ring) + " k " + std::to_string(k) + " seed " +
                         std::to_string(seed - 1));
            Substrate q(1000, ring);
            q.set_front_size(k);
            EXPECT_EQ(q.front_size(), k);
            std::vector<float> scores;
            const int          n = 1 + static_cast<int>(rng.next_below(200));
            for (int i = 0; i < n; ++i) {
                // A few distinct values so score ties are common.
                const float s = static_cast<float>(rng.next_below(9)) / 8.0F;
                q.push(static_cast<NodeId>(i), rng.next_below(1000), s);
                scores.push_back(s);
            }
            std::sort(scores.begin(), scores.end(), std::greater<float>());
            const auto front = q.current_front();
            ASSERT_EQ(front.size(), std::min<std::size_t>(k, static_cast<std::size_t>(n)));
            for (std::size_t i = 0; i < front.size(); ++i) {
                ASSERT_EQ(front[i].score, scores[i]) << "front position " << i;
            }
        }
    }
}

// The 64-bit-weight instantiation takes the same code path; a smoke test that
// it compiles and orders correctly with a ring that is not the default.
TEST(BucketRing, SixtyFourBitWeightInstantiationOrdersKeys) {
    BucketSubstrate<std::uint64_t> q(3, 16);
    q.push(1, 100);
    q.push(2, 0);
    q.push(3, 47);
    q.push(4, 48);
    q.push(5, 5000);
    for (std::uint64_t expected : {0ULL, 47ULL, 48ULL, 100ULL, 5000ULL}) {
        EXPECT_EQ(q.pop().key, expected);
    }
}

// =============================================================================
// 3. Queues through dijkstra() against the heap reference
// =============================================================================

// A deterministic pseudo-random score per node, with no per-graph state. The
// scores carry no information about keys, which is the point: a wrong score may
// change which entry is examined first and must never change the answer.
class HashScorer {
public:
    explicit HashScorer(std::uint64_t seed) : seed_(seed) {}
    [[nodiscard]] float score_of(NodeId u) const noexcept {
        std::uint64_t z = seed_ + 0x9E3779B97F4A7C15ULL * (static_cast<std::uint64_t>(u) + 1U);
        z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z               = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1U << 24);
    }

private:
    std::uint64_t seed_;
};

static_assert(NodeScorer<HashScorer>);

using ConstLpq = LearnedPriorityQueue<Weight, ConstantNodeScorer>;
using HashLpq  = LearnedPriorityQueue<Weight, HashScorer>;
static_assert(PriorityQueueLike<ConstLpq>);
static_assert(PriorityQueueLike<HashLpq>);

CandidateWindowConfig window_for(std::uint32_t front) {
    CandidateWindowConfig c;
    c.k_min          = 2;
    c.k_max          = std::max<std::uint32_t>(2, front);
    c.max_expansions = 2;
    return c;
}

// --- Graph families -----------------------------------------------------------

Graph build_graph(std::size_t n, std::vector<InputEdge<Weight>> edges, Directedness d,
                  const std::string& id) {
    GraphBuildOptions opts;
    opts.directedness = d;
    opts.dataset_id   = id;
    return GraphBuilder<Weight>::build(n, std::move(edges), opts);
}

Weight draw_weight(Rng& rng, int kind) {
    switch (kind) {
        case 0: return 1;
        case 1: return static_cast<Weight>(rng.next_in_range(1, 20));
        case 2: return static_cast<Weight>(rng.next_in_range(0, 20));  // zero-weight arcs
        case 3: return rng.next_below(3) == 0 ? static_cast<Weight>(rng.next_in_range(1, 200000))
                                              : static_cast<Weight>(rng.next_in_range(1, 5));
        case 5: {
            // Aligned to the widest bucket width used below (1000): path costs
            // then land exactly on bucket boundaries, with a few small offsets
            // that put several distinct keys inside one bucket.
            static constexpr Weight kOffsets[] = {0, 0, 1, 5, 7, 8};
            return static_cast<Weight>(1000U * rng.next_in_range(1, 3) + kOffsets[rng.next_below(6)]);
        }
        default: return static_cast<Weight>(rng.next_in_range(1, 1000));
    }
}

// m arcs drawn with replacement, so parallel arcs and self-loops occur
// naturally; a duplicate of an existing arc with another weight is injected too.
Graph sparse_random(Rng& rng, std::size_t n, std::size_t m, int weight_kind, bool undirected,
                    const std::string& id) {
    std::vector<InputEdge<Weight>> edges;
    for (std::size_t i = 0; i < m; ++i) {
        const NodeId u = rng.next_node(n);
        const NodeId v = rng.next_node(n);
        edges.push_back({u, v, draw_weight(rng, weight_kind)});
        if (rng.next_below(8) == 0) {
            edges.push_back({u, v, draw_weight(rng, weight_kind)});  // parallel arc
        }
    }
    return build_graph(n, std::move(edges), undirected ? Directedness::kUndirected
                                                       : Directedness::kDirected, id);
}

Graph weighted_grid(Rng& rng, std::size_t rows, std::size_t cols, Weight wmin, Weight wmax,
                    bool with_coordinates) {
    const std::size_t              n = rows * cols;
    std::vector<InputEdge<Weight>> edges;
    std::vector<Coordinate>        coords;
    if (with_coordinates) {
        coords.resize(n);
    }
    auto id = [cols](std::size_t r, std::size_t c) { return static_cast<NodeId>(r * cols + c); };
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            if (with_coordinates) {
                coords[id(r, c)] = Coordinate{static_cast<double>(c), static_cast<double>(r)};
            }
            if (c + 1 < cols) {
                edges.push_back({id(r, c), id(r, c + 1),
                                 static_cast<Weight>(rng.next_in_range(wmin, wmax))});
            }
            if (r + 1 < rows) {
                edges.push_back({id(r, c), id(r + 1, c),
                                 static_cast<Weight>(rng.next_in_range(wmin, wmax))});
            }
        }
    }
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    opts.coord_system = with_coordinates ? CoordinateSystem::kEuclidean : CoordinateSystem::kNone;
    opts.dataset_id   = "agent:weighted_grid";
    return GraphBuilder<Weight>::build(n, std::move(edges), opts, std::move(coords));
}

struct RefQuery {
    NodeId                source = 0;
    NodeId                target = kInvalidNode;
    ShortestPathResult<Weight> expected;
};

struct RefCase {
    std::string        name;
    Graph              graph;
    std::vector<RefQuery> queries;
};

RefCase make_case(std::string name, Graph g, Rng& rng) {
    RefCase c;
    c.name  = std::move(name);
    c.graph = std::move(g);
    const std::size_t n = c.graph.num_nodes();
    if (n == 0) {
        return c;
    }
    for (int s = 0; s < 2; ++s) {
        const NodeId src = rng.next_node(n);
        // Full single-source, then single-target runs: a random node (which in
        // sparse families is often unreachable), and the source itself.
        for (int t = 0; t < 3; ++t) {
            RefQuery q;
            q.source = src;
            q.target = t == 0 ? kInvalidNode : (t == 1 ? rng.next_node(n) : src);
            ReferenceDijkstraOptions ro;
            ro.target  = q.target;
            q.expected = reference_dijkstra_heap<Weight>(c.graph, src, ro);
            c.queries.push_back(std::move(q));
        }
    }
    return c;
}

std::vector<RefCase> make_cases(std::uint64_t seed) {
    Rng               rng(seed);
    std::vector<RefCase> cases;
    const std::string tag = "seed" + std::to_string(seed) + ":";

    for (int i = 0; i < 18; ++i) {  // random sparse, every weight regime
        const std::size_t n = static_cast<std::size_t>(rng.next_in_range(2, 300));
        const std::size_t m = n * static_cast<std::size_t>(rng.next_in_range(1, 4));
        const int         kind = i % 6;
        cases.push_back(make_case(tag + "sparse" + std::to_string(i) + "/w" + std::to_string(kind),
                                  sparse_random(rng, n, m, kind, i % 2 == 0, "agent:sparse"), rng));
    }
    for (int i = 0; i < 4; ++i) {  // mostly unreachable
        const std::size_t n = static_cast<std::size_t>(rng.next_in_range(20, 200));
        cases.push_back(make_case(tag + "unreachable" + std::to_string(i),
                                  sparse_random(rng, n, n / 2, 1, false, "agent:unreach"), rng));
    }
    for (int i = 0; i < 6; ++i) {  // grids
        const std::size_t r = static_cast<std::size_t>(rng.next_in_range(1, 18));
        const std::size_t c = static_cast<std::size_t>(rng.next_in_range(2, 18));
        const Weight      lo = i % 3 == 2 ? 0 : 1;
        cases.push_back(make_case(tag + "grid" + std::to_string(i),
                                  weighted_grid(rng, r, c, lo, i < 2 ? 1 : 30, false), rng));
    }
    for (Weight w : {Weight{0}, Weight{1}, Weight{7}, Weight{5000}}) {  // paths
        const std::size_t n = static_cast<std::size_t>(rng.next_in_range(2, 300));
        cases.push_back(make_case(tag + "path/w" + std::to_string(w),
                                  lpq_test::path_graph<Weight>(n, w), rng));
    }
    for (Weight w : {Weight{0}, Weight{1}, Weight{3}, Weight{2000}}) {  // stars
        const std::size_t n = static_cast<std::size_t>(rng.next_in_range(2, 300));
        cases.push_back(make_case(tag + "star/w" + std::to_string(w),
                                  lpq_test::star_graph<Weight>(n, w), rng));
    }
    cases.push_back(make_case(tag + "zero_weight", lpq_test::zero_weight_graph<Weight>(80), rng));
    cases.push_back(make_case(tag + "duplicates", lpq_test::duplicate_edge_graph(), rng));
    cases.push_back(make_case(tag + "disconnected", lpq_test::disconnected_graph<Weight>(6), rng));
    cases.push_back(make_case(tag + "diamond", lpq_test::diamond_graph(), rng));
    {   // the library's own fuzz generator, with ties and zero weights
        for (int i = 0; i < 6; ++i) {
            lpq_test::RandomGraphSpec spec;
            spec.min_nodes         = 2;
            spec.max_nodes         = 50;
            spec.edge_probability  = 0.1;
            spec.max_weight        = 60;
            spec.many_ties         = (i % 2 == 0);
            spec.allow_zero_weights = (i % 3 == 0);
            cases.push_back(make_case(tag + "fuzz" + std::to_string(i),
                                      lpq_test::random_graph(rng, spec), rng));
        }
    }
    return cases;
}

template <typename Queue>
void run_cases(Queue& queue, const std::vector<RefCase>& cases, const std::string& label) {
    for (const RefCase& c : cases) {
        for (const RefQuery& q : c.queries) {
            DijkstraOptions o;
            o.target       = q.target;
            o.measure_time = false;
            const auto got = dijkstra<Weight>(c.graph, q.source, queue, o);
            const std::string ctx = label + " | " + c.name + " | source " +
                                    std::to_string(q.source) + " target " +
                                    (q.target == kInvalidNode ? std::string("all")
                                                              : std::to_string(q.target));
            lpq_test::expect_results_agree(c.graph, q.expected, got, ctx);
            if (q.target == kInvalidNode) {
                lpq_test::expect_no_relaxable_arc(c.graph, got, ctx);
            }
            if (::testing::Test::HasFailure()) {
                return;  // one failing configuration is enough to report
            }
        }
    }
}

constexpr Key kQueueWidths[] = {1, 3, 37, 1000};

TEST(ClassicalBucketQueue, MatchesTheHeapReferenceAcrossWidthsAndRings) {
    const auto cases = make_cases(5100001);
    for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{1024}, std::size_t{16384}}) {
        for (Key width : kQueueWidths) {
            Classical q(width, ring);
            run_cases(q, cases, "classical w=" + std::to_string(width) + " ring=" + std::to_string(ring));
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

TEST(ClassicalBucketQueue, SecondSeedAndTheDefaultRing) {
    const auto cases = make_cases(5100002);
    for (Key width : kQueueWidths) {
        Classical q(width);  // default ring
        EXPECT_EQ(q.substrate().ring_size(), Substrate::kBucketCount);
        run_cases(q, cases, "classical default-ring w=" + std::to_string(width));
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(ClassicalBucketQueue, PopOnAnEmptyQueueThrows) {
    Classical q(4, 64);
    EXPECT_THROW((void)q.pop(), InternalError);
    EXPECT_THROW((void)q.top(), InternalError);
}

template <typename Scorer>
void run_direct_matrix(const std::vector<RefCase>& cases, Scorer& scorer, const std::string& scorer_name,
                       bool attach_scorer) {
    // Rings of 2 and 4 make overflow and redistribution the common case rather
    // than the exception; the larger rings are the production-sized ones.
    for (std::size_t ring : {std::size_t{2}, std::size_t{4}, std::size_t{64}, std::size_t{1024},
                             std::size_t{16384}}) {
        for (Key width : kQueueWidths) {
            for (std::uint32_t front : {0U, 1U, 4U, 32U}) {
                LearnedPriorityQueue<Weight, Scorer> q(width, window_for(front),
                                                       CorrectnessPolicy::exact(),
                                                       ExtractionStrategy::kDirect, ring);
                q.set_front_size(front);
                if (attach_scorer) {
                    q.set_scorer(&scorer);
                }
                const std::string label = "kDirect " + scorer_name + " w=" + std::to_string(width) +
                                          " ring=" + std::to_string(ring) + " front=" +
                                          std::to_string(front);
                run_cases(q, cases, label);
                if (::testing::Test::HasFailure()) {
                    return;
                }
                EXPECT_EQ(q.metrics().window_divergences, 0U) << label;
            }
        }
    }
}

TEST(DirectExtraction, ConstantScorerMatchesTheHeapReference) {
    const auto         cases = make_cases(5200001);
    ConstantNodeScorer scorer;
    run_direct_matrix(cases, scorer, "constant", true);
}

TEST(DirectExtraction, PseudoRandomScorerMatchesTheHeapReference) {
    const auto cases = make_cases(5200002);
    HashScorer scorer(0xA5A5A5A5ULL);
    run_direct_matrix(cases, scorer, "hash", true);
}

TEST(DirectExtraction, NoScorerAttachedMatchesTheHeapReference) {
    const auto         cases = make_cases(5200003);
    ConstantNodeScorer unused;
    run_direct_matrix(cases, unused, "no-scorer", false);
}

TEST(DirectExtraction, ASecondPseudoRandomScorerSeedOnAFreshGraphSet) {
    const auto cases = make_cases(5200004);
    HashScorer scorer(12345ULL);
    run_direct_matrix(cases, scorer, "hash2", true);
}

TEST(DirectExtraction, DefaultConstructedStrategyIsModularAndDirectIsOptIn) {
    ConstLpq modular;
    EXPECT_EQ(modular.strategy(), ExtractionStrategy::kModular);
    ConstLpq direct(1, window_for(4), CorrectnessPolicy::exact(), ExtractionStrategy::kDirect);
    EXPECT_EQ(direct.strategy(), ExtractionStrategy::kDirect);
    EXPECT_EQ(direct.substrate().ring_size(), Substrate::kBucketCount);
}

TEST(DirectExtraction, RejectsInvalidRingSizes) {
    for (std::size_t bad : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{1} << 23}) {
        EXPECT_THROW(ConstLpq(1, window_for(4), CorrectnessPolicy::exact(),
                              ExtractionStrategy::kDirect, bad),
                     InvalidConfigError)
            << "ring " << bad;
        EXPECT_THROW(ConstLpq(1, window_for(4), CorrectnessPolicy::exact(),
                              ExtractionStrategy::kModular, bad),
                     InvalidConfigError)
            << "ring " << bad;
    }
}

TEST(DirectExtraction, ApproximatePolicyIsRefused) {
    for (double eps : {0.01, 0.1, 1.0}) {
        EXPECT_THROW(ConstLpq(4, window_for(4), CorrectnessPolicy::approximate(eps),
                              ExtractionStrategy::kDirect),
                     InvalidConfigError)
            << "epsilon " << eps;
    }
    // The policy object is not required to have been validated to be refused:
    // any slack other than exactly 1 is approximation.
    CorrectnessPolicy hand_built;
    hand_built.epsilon = 0.5;
    EXPECT_THROW(ConstLpq(4, window_for(4), hand_built, ExtractionStrategy::kDirect),
                 InvalidConfigError);

    // Controls: the modular strategy still accepts an approximate policy, and
    // kDirect accepts the exact one. Without these the throws above could be
    // an unrelated construction failure.
    EXPECT_NO_THROW(ConstLpq(4, window_for(4), CorrectnessPolicy::approximate(0.1),
                             ExtractionStrategy::kModular));
    EXPECT_NO_THROW(ConstLpq(4, window_for(4), CorrectnessPolicy::exact(),
                             ExtractionStrategy::kDirect));
}

TEST(DirectExtraction, PopOnAnEmptyQueueThrowsInternalError) {
    ConstLpq q(4, window_for(4), CorrectnessPolicy::exact(), ExtractionStrategy::kDirect, 64);
    EXPECT_THROW((void)q.pop(), InternalError);
    q.push(1, 10);
    EXPECT_EQ(q.pop().key, 10U);
    EXPECT_THROW((void)q.pop(), InternalError);
}

TEST(DirectExtraction, DrainsInKeyOrderAgainstAnOracleWithEveryFrontSize) {
    std::uint64_t seed = 6100000;
    for (std::uint32_t front : {0U, 1U, 4U, 32U}) {
        for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{1024}}) {
            for (Key width : kQueueWidths) {
                Rng       rng(seed++);
                HashScorer scorer(seed);
                HashLpq   q(width, window_for(front), CorrectnessPolicy::exact(),
                            ExtractionStrategy::kDirect, ring);
                q.set_front_size(front);
                q.set_scorer(&scorer);
                SCOPED_TRACE("front " + std::to_string(front) + " ring " + std::to_string(ring) +
                             " width " + std::to_string(width) + " seed " +
                             std::to_string(seed - 1));

                std::multiset<std::pair<Key, NodeId>> items;
                Key last_key = 0;
                for (int op = 0; op < 3000; ++op) {
                    if (items.empty() || rng.next_below(100) < (op % 400 < 200 ? 70U : 35U)) {
                        const Key span = static_cast<Key>(ring) * width;
                        Key key = last_key;
                        switch (rng.next_below(5)) {
                            case 0: break;
                            case 1: key += rng.next_below(width + 1); break;
                            case 2: key += rng.next_below(span + 1); break;
                            case 3: key += span + rng.next_below(span + 1); break;
                            default: key += rng.next_below(width * 4 + 1); break;
                        }
                        const NodeId node = static_cast<NodeId>(rng.next_below(64));
                        q.push(node, key);
                        items.emplace(key, node);
                    } else {
                        const auto e = q.pop();
                        ASSERT_EQ(e.key, items.begin()->first) << "op " << op;
                        const auto it = items.find({e.key, e.node});
                        ASSERT_NE(it, items.end()) << "op " << op;
                        items.erase(it);
                        last_key = e.key;
                    }
                    ASSERT_EQ(q.size(), items.size());
                }
                while (!items.empty()) {
                    const auto e = q.pop();
                    ASSERT_EQ(e.key, items.begin()->first);
                    const auto it = items.find({e.key, e.node});
                    ASSERT_NE(it, items.end());
                    items.erase(it);
                }
                ASSERT_TRUE(q.empty());
            }
        }
    }
}

// The early exit on the bound is the whole point of kDirect: equal keys in a
// wide bucket should be accepted without scanning the bucket. Counts, not
// timings, so the check is deterministic.
TEST(DirectExtraction, EarlyExitOnTheBoundDoesLessWorkThanAFullScan) {
    ConstantNodeScorer scorer;
    ConstLpq direct(64, window_for(4), CorrectnessPolicy::exact(), ExtractionStrategy::kDirect, 64);
    direct.set_scorer(&scorer);
    Classical classical(64, 64);

    for (NodeId u = 0; u < 200; ++u) {
        direct.push(u, 5);
        classical.push(u, 5);
    }
    direct.reset_comparison_count();
    classical.reset_comparison_count();
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(direct.pop().key, 5U);
        EXPECT_EQ(classical.pop().key, 5U);
    }
    EXPECT_LT(direct.comparison_count(), classical.comparison_count())
        << "kDirect did not examine fewer entries than the full scan on an all-equal bucket";
    EXPECT_GT(direct.metrics().entries_skipped, 0U);
    EXPECT_EQ(direct.metrics().window_divergences, 0U);
}

// --- Observers ----------------------------------------------------------------

// Steers the extraction through a scripted policy and records what it is told.
class ScriptedObserver final : public ExtractionObserver {
public:
    enum class Mode { kAlwaysClassical, kAlternate, kTinyWindow, kZeroWindow, kLearnedNormal };
    explicit ScriptedObserver(Mode m) : mode_(m) {}

    [[nodiscard]] ExtractionPolicy policy() const noexcept override {
        ++policy_calls;
        ExtractionPolicy p;
        switch (mode_) {
            case Mode::kAlwaysClassical: p.use_learned_path = false; break;
            case Mode::kAlternate: p.use_learned_path = (policy_calls % 2 == 0); break;
            case Mode::kTinyWindow: p.window_size = 1; break;
            case Mode::kZeroWindow: p.window_size = 0; break;
            case Mode::kLearnedNormal: break;
        }
        return p;
    }

    void on_extraction(std::uint64_t skipped, std::uint32_t /*comparisons*/,
                       const CorrectionOutcome& /*outcome*/) noexcept override {
        ++extractions;
        total_skipped += skipped;
    }

    mutable std::uint64_t policy_calls = 0;
    std::uint64_t         extractions  = 0;
    std::uint64_t         total_skipped = 0;

private:
    Mode mode_;
};

TEST(DirectExtraction, AnObserverRetreatingToTheClassicalScanStillGivesExactAnswers) {
    const auto cases = make_cases(5300001);
    HashScorer scorer(99U);
    for (std::size_t ring : {std::size_t{1024}, std::size_t{16384}}) {
        for (Key width : {Key{1}, Key{37}, Key{1000}}) {
            for (std::uint32_t front : {0U, 4U, 32U}) {
                ScriptedObserver obs(ScriptedObserver::Mode::kAlwaysClassical);
                HashLpq q(width, window_for(front), CorrectnessPolicy::exact(),
                          ExtractionStrategy::kDirect, ring);
                q.set_front_size(front);
                q.set_scorer(&scorer);
                q.set_observer(&obs);
                const std::string label = "observer-retreat w=" + std::to_string(width) +
                                          " ring=" + std::to_string(ring) + " front=" +
                                          std::to_string(front);
                run_cases(q, cases, label);
                if (::testing::Test::HasFailure()) {
                    return;
                }
                // Every extraction took the fallback, and none claimed savings.
                EXPECT_GT(obs.extractions, 0U) << label;
                EXPECT_EQ(obs.total_skipped, 0U) << label;
                EXPECT_EQ(obs.policy_calls, obs.extractions) << label;
            }
        }
    }
}

TEST(DirectExtraction, RetreatCountsEveryExtractionAsAFallbackScan) {
    ConstantNodeScorer scorer;
    ScriptedObserver   obs(ScriptedObserver::Mode::kAlwaysClassical);
    ConstLpq q(8, window_for(4), CorrectnessPolicy::exact(), ExtractionStrategy::kDirect, 64);
    q.set_scorer(&scorer);
    q.set_observer(&obs);
    for (NodeId u = 0; u < 100; ++u) {
        q.push(u, 3);
    }
    while (!q.empty()) {
        EXPECT_EQ(q.pop().key, 3U);
    }
    EXPECT_EQ(q.metrics().extractions, 100U);
    EXPECT_EQ(q.metrics().fallback_scans, 100U);
    EXPECT_EQ(q.metrics().verified_extractions, 0U);
}

TEST(DirectExtraction, OtherObserverPoliciesStayExact) {
    const auto cases = make_cases(5300002);
    HashScorer scorer(7U);
    for (auto mode : {ScriptedObserver::Mode::kAlternate, ScriptedObserver::Mode::kTinyWindow,
                      ScriptedObserver::Mode::kZeroWindow, ScriptedObserver::Mode::kLearnedNormal}) {
        for (Key width : {Key{1}, Key{37}, Key{1000}}) {
            for (std::uint32_t front : {0U, 1U, 4U, 32U}) {
                ScriptedObserver obs(mode);
                HashLpq q(width, window_for(front), CorrectnessPolicy::exact(),
                          ExtractionStrategy::kDirect, 1024);
                q.set_front_size(front);
                q.set_scorer(&scorer);
                q.set_observer(&obs);
                const std::string label = "observer-mode=" + std::to_string(static_cast<int>(mode)) +
                                          " w=" + std::to_string(width) + " front=" +
                                          std::to_string(front);
                run_cases(q, cases, label);
                if (::testing::Test::HasFailure()) {
                    return;
                }
                EXPECT_GT(obs.extractions, 0U) << label;
            }
        }
    }
}

// =============================================================================
// 4. A* with a consistent heuristic
// =============================================================================

template <typename Heuristic, typename Queue>
void expect_astar_matches(Queue& queue, const Graph& g, NodeId source, NodeId target,
                          const std::string& label) {
    ReferenceDijkstraOptions ro;
    ro.target = target;
    const auto expected = reference_dijkstra_heap<Weight>(g, source, ro);

    Heuristic    h;
    AstarOptions ao;
    ao.target       = target;
    ao.measure_time = false;
    const auto got  = astar<Weight, Queue, Heuristic>(g, source, queue, h, ao);

    const std::string ctx = label + " source " + std::to_string(source) + " target " +
                            std::to_string(target);
    EXPECT_EQ(got.dist[target], expected.dist[target]) << ctx;
    lpq_test::expect_results_agree(g, expected, got, ctx);
}

template <typename Queue>
void astar_sweep(Queue& queue, const std::vector<Graph>& grids, std::uint64_t seed,
                 const std::string& label) {
    Rng rng(seed);
    for (std::size_t gi = 0; gi < grids.size(); ++gi) {
        const Graph& g = grids[gi];
        for (int q = 0; q < 6; ++q) {
            const NodeId s = rng.next_node(g.num_nodes());
            const NodeId t = rng.next_node(g.num_nodes());
            const std::string l = label + " grid#" + std::to_string(gi);
            expect_astar_matches<EuclideanHeuristic<Weight>>(queue, g, s, t, l + " euclid");
            expect_astar_matches<ManhattanHeuristic<Weight>>(queue, g, s, t, l + " manhattan");
            expect_astar_matches<ZeroHeuristic<Weight>>(queue, g, s, t, l + " zero");
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

std::vector<Graph> astar_grids() {
    Rng                rng(5400001);
    std::vector<Graph> grids;
    grids.push_back(lpq_test::grid_graph<Weight>(15, 15));      // unit weights, coordinates
    grids.push_back(lpq_test::grid_graph<Weight>(9, 22, 3));    // uniform weight 3 (>= unit spacing)
    grids.push_back(weighted_grid(rng, 20, 20, 1, 9, true));
    grids.push_back(weighted_grid(rng, 12, 31, 1, 400, true));  // spread wider than small rings
    grids.push_back(weighted_grid(rng, 1, 40, 1, 5, true));     // a single row
    grids.push_back(weighted_grid(rng, 25, 25, 1, 3, true));
    return grids;
}

TEST(AstarBucketQueues, ClassicalBucketQueueMatchesTheReference) {
    const auto grids = astar_grids();
    for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{1024}}) {
        for (Key width : {Key{1}, Key{5}, Key{64}}) {
            Classical q(width, ring);
            astar_sweep(q, grids, 5400100 + ring + width,
                        "classical w=" + std::to_string(width) + " ring=" + std::to_string(ring));
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

TEST(AstarBucketQueues, DirectLpqMatchesTheReference) {
    const auto grids = astar_grids();
    HashScorer scorer(424242ULL);
    for (std::size_t ring : {std::size_t{64}, std::size_t{1024}, std::size_t{16384}}) {
        for (Key width : {Key{1}, Key{5}, Key{64}}) {
            for (std::uint32_t front : {0U, 1U, 4U, 32U}) {
                HashLpq q(width, window_for(front), CorrectnessPolicy::exact(),
                          ExtractionStrategy::kDirect, ring);
                q.set_front_size(front);
                q.set_scorer(&scorer);
                astar_sweep(q, grids, 5400200 + ring + width + front,
                            "direct w=" + std::to_string(width) + " ring=" +
                                std::to_string(ring) + " front=" + std::to_string(front));
                if (::testing::Test::HasFailure()) {
                    return;
                }
                EXPECT_EQ(q.metrics().window_divergences, 0U);
            }
        }
    }
}

TEST(AstarBucketQueues, DirectLpqWithARetreatingObserverMatchesTheReference) {
    const auto grids = astar_grids();
    ConstantNodeScorer scorer;
    ScriptedObserver   obs(ScriptedObserver::Mode::kAlternate);
    ConstLpq q(5, window_for(4), CorrectnessPolicy::exact(), ExtractionStrategy::kDirect, 1024);
    q.set_front_size(4);
    q.set_scorer(&scorer);
    q.set_observer(&obs);
    astar_sweep(q, grids, 5400300, "direct+alternating-observer");
}

// =============================================================================
// 5. Regression: seeds and cases that ever produced a failure live here, permanently.
//
// 2026-10-11, found by the first run of BucketRing.RandomisedMonotoneWorkloads...
//
//   Symptom: BucketSubstrate::current_bound() returned a value ABOVE the true
//   minimum of the queue, which the bound contract forbids. The first revision
//   of the harness (one queue reused across trials, so NOT replayable from the
//   seed alone) failed at ring 64, width 1000, seed 7100209, trial 13: bound
//   2200518449798 against a queued key 2200518449366.
//
//   Cause (bucket_substrate.hpp, advance_to_non_empty()): when the ring is empty
//   the function calls redistribute_overflow(), which re-files the overflow
//   entries and leaves cursor_ pointing at a now-POPULATED bucket. The loop then
//   finds next == cursor_ and still executes `bucket_bound_[cursor_] = kNoBound`
//   -- intended for the bucket being LEFT, here wiping the bound of the bucket
//   being ENTERED. A later push() into that bucket sees kNoBound and records its
//   own key as the bound, ignoring entries already there, so the cached bound
//   exceeds the bucket's minimum.
//
//   Consequence: kDirect accepts the first entry <= bound and returns a
//   non-minimal key (ExtractionStrategy::kDirect pops 1007 before 1005 below),
//   which makes a single-target Dijkstra settle a node at a non-final distance.
//   The cases below are the smallest inputs found that show each stage.
//
//   Not in the pre-change code, whose loop re-tested buckets_[cursor_].empty()
//   after a redistribute and never wiped a populated bucket.
//
// The seeds below are replayable alone (each trial builds its own queue).
// =============================================================================

TEST(Regression, RedistributeThenPushMustNotRaiseTheBoundAboveAQueuedKey) {
    for (std::size_t ring : {std::size_t{2}, std::size_t{4}, std::size_t{64}}) {
        for (Key width : {Key{10}, Key{37}, Key{1000}}) {
            SCOPED_TRACE("ring " + std::to_string(ring) + " width " + std::to_string(width));
            const Key lowest = 100 * width * ring + 3;  // far beyond the ring: parked in overflow
            Substrate q(width, ring);
            q.push(1, 0);
            q.push(2, lowest);
            ASSERT_EQ(q.pop().key, 0U);
            // Advancing now redistributes the overflow entry into the ring.
            ASSERT_LE(q.current_bound(), lowest);
            q.push(3, lowest + 4);  // same bucket, larger key
            EXPECT_LE(q.current_bound(), lowest) << "bound is above a queued key";
            EXPECT_EQ(q.pop().key, lowest);
            EXPECT_EQ(q.pop().key, lowest + 4);
        }
    }
}

TEST(Regression, DirectExtractionPopOrderAfterARedistribute) {
    for (auto strategy : {ExtractionStrategy::kDirect, ExtractionStrategy::kModular}) {
        for (Key width : {Key{10}, Key{37}, Key{1000}}) {
            SCOPED_TRACE("strategy " + std::to_string(static_cast<int>(strategy)) + " width " +
                         std::to_string(width));
            // A is a bucket boundary, so the first redistributed entry has key == base
            // and kDirect accepts it on the bound alone, leaving the bound wiped.
            const Key A = 100 * width;
            ConstLpq q(width, window_for(2), CorrectnessPolicy::exact(), strategy, 2);
            q.set_front_size(0);
            q.push(1, 0);
            q.push(2, A);      // overflow, in this order: A, A+5, A+7
            q.push(3, A + 5);
            q.push(4, A + 7);
            ASSERT_EQ(q.pop().key, 0U);
            ASSERT_EQ(q.pop().key, A);
            q.push(5, A + 8);  // first push after the wipe: records itself as the bound
            for (Key expected : {A + 5, A + 7, A + 8}) {
                ASSERT_FALSE(q.empty());
                EXPECT_EQ(q.pop().key, expected) << "extraction returned a non-minimal key";
            }
            EXPECT_TRUE(q.empty());
        }
    }
}

TEST(Regression, DirectDijkstraSingleTargetAfterARedistribute) {
    for (Key width : {Key{10}, Key{37}, Key{1000}}) {
        SCOPED_TRACE("width " + std::to_string(width));
        const Weight A = static_cast<Weight>(100 * width);
        // 3 is reachable at A+7 directly and at A+6 through 2.
        std::vector<InputEdge<Weight>> edges{{0, 1, A}, {0, 2, A + 5}, {0, 3, A + 7},
                                             {1, 4, 8}, {2, 3, 1}};
        const Graph g = build_graph(5, std::move(edges), Directedness::kDirected, "regression");
        for (NodeId target : {NodeId{3}, kInvalidNode}) {
            ReferenceDijkstraOptions ro;
            ro.target = target;
            const auto expected = reference_dijkstra_heap<Weight>(g, 0, ro);
            ASSERT_EQ(expected.dist[3], A + 6U);

            ConstLpq q(width, window_for(2), CorrectnessPolicy::exact(),
                       ExtractionStrategy::kDirect, 2);
            q.set_front_size(0);
            DijkstraOptions o;
            o.target       = target;
            o.measure_time = false;
            const auto got = dijkstra<Weight>(g, 0, q, o);
            lpq_test::expect_results_agree(g, expected, got,
                                           "kDirect ring 2 width " + std::to_string(width));
        }
    }
}

// Seeds from BucketRing.RandomisedMonotoneWorkloadsMatchAnOracleAcrossRingsAndWidths
// that failed on 2026-10-11 against the header described above. Each replays
// alone. Kept forever; do not remove after the header is fixed.
TEST(Regression, SubstrateOracleSeed7100194Ring64Width37) {
    ASSERT_NO_FATAL_FAILURE(substrate_trial(37, 64, 7100194));
}

TEST(Regression, SubstrateOracleSeed7100253Ring1024Width37) {
    ASSERT_NO_FATAL_FAILURE(substrate_trial(37, 1024, 7100253));
}


// =============================================================================
// 5. PooledBucketQueue and the queue-selection policy
// =============================================================================

using Pooled = PooledBucketQueue<Weight>;
static_assert(PriorityQueueLike<Pooled>);
static_assert(LazyDeletionQueue<Pooled>);

TEST(PooledBucketQueue, MatchesTheHeapReferenceAcrossWidthsAndRings) {
    const auto cases = make_cases(5200001);
    for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{1024}, std::size_t{16384}}) {
        for (Key width : kQueueWidths) {
            Pooled q(width, ring);
            run_cases(q, cases, "pooled w=" + std::to_string(width) + " ring=" + std::to_string(ring));
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

TEST(PooledBucketQueue, AStarMatchesTheReference) {
    const auto grids = astar_grids();
    for (std::size_t ring : {std::size_t{2}, std::size_t{64}, std::size_t{1024}}) {
        for (Key width : {Key{1}, Key{5}, Key{64}}) {
            Pooled q(width, ring);
            astar_sweep(q, grids, 5400200 + ring + width,
                        "pooled w=" + std::to_string(width) + " ring=" + std::to_string(ring));
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

// Random monotone push/pop streams against a priority_queue oracle. Keys are
// drawn from small steps, window-edge steps and enormous jumps so every route
// through overflow and refile is taken; the popped KEY sequence must equal the
// oracle's and every pushed (node, key) must come out exactly once.
void pooled_oracle_trial(Key width, std::size_t ring, std::uint64_t seed) {
    SCOPED_TRACE("pooled oracle width " + std::to_string(width) + " ring " + std::to_string(ring) +
                 " seed " + std::to_string(seed));
    Rng    rng(seed);
    Pooled q(width, ring);
    std::priority_queue<Key, std::vector<Key>, std::greater<Key>> oracle;
    std::multiset<std::pair<NodeId, Key>> in_queue;
    Key floor_key = 0;
    NodeId next_node = 0;

    const auto push_one = [&] {
        const std::uint64_t kind = rng.next_below(10);
        Key step;
        if (kind < 6) step = rng.next_below(width * 3 + 2);
        else if (kind < 8) step = width * ring - 2 + rng.next_below(5);   // window edge
        else if (kind < 9) step = rng.next_below(width * ring * 4 + 2);
        else step = width * ring * (10 + rng.next_below(50)) + rng.next_below(1000);
        const Key key = floor_key + step;
        q.push(next_node, key);
        oracle.push(key);
        in_queue.insert({next_node, key});
        ++next_node;
    };
    for (int op = 0; op < 3000; ++op) {
        if (q.empty() || rng.next_below(100) < 55) {
            push_one();
        } else {
            const auto e = q.pop();
            ASSERT_EQ(e.key, oracle.top()) << "op " << op;
            oracle.pop();
            const auto it = in_queue.find({e.node, e.key});
            ASSERT_NE(it, in_queue.end()) << "popped an entry that was never pushed";
            in_queue.erase(it);
            floor_key = e.key;
            ASSERT_EQ(q.size(), oracle.size());
        }
    }
    while (!q.empty()) {
        const auto e = q.pop();
        ASSERT_EQ(e.key, oracle.top());
        oracle.pop();
        const auto it = in_queue.find({e.node, e.key});
        ASSERT_NE(it, in_queue.end());
        in_queue.erase(it);
    }
    ASSERT_TRUE(in_queue.empty());
}

TEST(PooledBucketQueue, RandomisedMonotoneWorkloadsMatchAnOracleAcrossRingsAndWidths) {
    std::uint64_t seed = 5300000;
    for (std::size_t ring : {std::size_t{2}, std::size_t{4}, std::size_t{64}, std::size_t{1024}}) {
        for (Key width : {Key{1}, Key{2}, Key{3}, Key{37}, Key{1000}}) {
            for (int rep = 0; rep < 12; ++rep) {
                ASSERT_NO_FATAL_FAILURE(pooled_oracle_trial(width, ring, ++seed));
            }
        }
    }
}

TEST(PooledBucketQueue, RejectsInvalidGeometryAndEmptyPop) {
    EXPECT_THROW(Pooled(0, 64), InvalidConfigError);
    EXPECT_THROW(Pooled(1, 0), InvalidConfigError);
    EXPECT_THROW(Pooled(1, 1), InvalidConfigError);
    EXPECT_THROW(Pooled(1, 3), InvalidConfigError);
    EXPECT_THROW(Pooled(1, Pooled::kMaxRing * 2), InvalidConfigError);
    Pooled q(1, 8);
    EXPECT_THROW((void)q.pop(), InternalError);
}

TEST(PooledBucketQueue, PushBelowTheBaseIsAnError) {
    Pooled q(4, 8);
    q.push(1, 100);
    (void)q.pop();
    q.push(2, 130);
    (void)q.pop();  // base has advanced past 100
    EXPECT_THROW(q.push(3, 50), InternalError);
}

TEST(PooledBucketQueue, ClearAllowsReuseAndKeepsOrder) {
    Pooled q(2, 16);
    for (int cycle = 0; cycle < 50; ++cycle) {
        q.clear();
        for (NodeId n = 0; n < 40; ++n) {
            q.push(n, static_cast<Key>((n * 7919U) % 500U));
        }
        Key last = 0;
        std::size_t count = 0;
        while (!q.empty()) {
            const auto e = q.pop();
            ASSERT_GE(e.key, last);
            last = e.key;
            ++count;
        }
        ASSERT_EQ(count, 40U);
    }
}

TEST(PooledBucketQueue, DuplicateNodesWithDifferentKeysAreBothReturned) {
    Pooled q(1, 8);
    q.push(7, 5);
    q.push(7, 3);
    q.push(7, 5);
    EXPECT_EQ(q.pop().key, 3U);
    EXPECT_EQ(q.pop().key, 5U);
    EXPECT_EQ(q.pop().key, 5U);
    EXPECT_TRUE(q.empty());
}

TEST(QueuePolicy, RingCoversTheLargestKeyRise) {
    // A push is at most max_step above the current bucket's top, so its offset
    // is at most 1 + max_step / width. The ring must exceed that.
    for (std::uint64_t step : {1ULL, 7ULL, 1000ULL, 36946ULL, 214013ULL}) {
        for (std::uint64_t width : {1ULL, 2ULL, 4ULL, 37ULL}) {
            const std::size_t ring = pooled_ring_for(step, width);
            EXPECT_TRUE(std::has_single_bit(ring));
            EXPECT_GT(ring, 1 + step / width) << "step " << step << " width " << width;
        }
    }
}

std::string write_temp(const std::string& name, const std::string& text) {
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream(path) << text;
    return path;
}

const char* kFeatureLine =
    "features log2_max_w log2_mean_w p50_over_max p99_over_max cv_w log2_nodes mean_degree\n";

WeightProfile sample_profile() {
    WeightProfile p;
    p.nodes = 264346; p.arcs = 733846; p.min_weight = 1; p.max_weight = 36946;
    p.mean_weight = 1293.3; p.stddev_weight = 1130.0; p.p50 = 1000; p.p90 = 2600; p.p99 = 5200;
    p.mean_degree = 2.776;
    return p;
}

TEST(QueuePolicy, NoPolicyMeansRadix) {
    const QueueSelectionPolicy pol;
    const QueueChoice c = pol.choose(sample_profile(), 36946);
    EXPECT_EQ(c.kind, QueueKind::kRadixHeap);
    EXPECT_EQ(c.describe(), "radix_heap");
}

TEST(QueuePolicy, ChoosesTheCandidatePredictedToBeatRadixByTheMargin) {
    const auto path = write_temp(
        "lpq_policy_choose.lpqpolicy",
        std::string("lpqpolicy 1\nmargin 0.05\ntrained_on unit test\n") + kFeatureLine +
            "candidate pooled w=1 intercept -0.10 coef 0 0 0 0 0 0 0\n"
            "candidate pooled w=2 intercept -0.20 coef 0 0 0 0 0 0 0\n"
            "candidate pooled w=4 intercept 0.30 coef 0 0 0 0 0 0 0\n");
    const auto pol = QueueSelectionPolicy::load(path);
    EXPECT_EQ(pol.candidate_count(), 3U);
    const QueueChoice c = pol.choose(sample_profile(), 36946);
    ASSERT_EQ(c.kind, QueueKind::kPooledBucket);
    EXPECT_EQ(c.width, 2U);                       // lowest predicted ratio
    EXPECT_NEAR(c.predicted_ratio, std::exp(-0.20), 1e-12);
    EXPECT_EQ(c.ring, pooled_ring_for(36946, 2));
    std::remove(path.c_str());
}

TEST(QueuePolicy, KeepsRadixWhenNoCandidateClearsTheMargin) {
    const auto path = write_temp(
        "lpq_policy_margin.lpqpolicy",
        std::string("lpqpolicy 1\nmargin 0.10\n") + kFeatureLine +
            "candidate pooled w=1 intercept -0.05 coef 0 0 0 0 0 0 0\n");  // ratio 0.951 > 0.90
    const auto pol = QueueSelectionPolicy::load(path);
    EXPECT_EQ(pol.choose(sample_profile(), 36946).kind, QueueKind::kRadixHeap);
    std::remove(path.c_str());
}

TEST(QueuePolicy, SkipsGeometriesWhoseRingWouldBeTooLarge) {
    const auto path = write_temp(
        "lpq_policy_ring.lpqpolicy",
        std::string("lpqpolicy 1\nmargin 0.0\n") + kFeatureLine +
            "candidate pooled w=1 intercept -1.0 coef 0 0 0 0 0 0 0\n");
    const auto pol = QueueSelectionPolicy::load(path);
    // max_step 10^6 at width 1 needs a ring far beyond the cap.
    EXPECT_EQ(pol.choose(sample_profile(), 1'000'000).kind, QueueKind::kRadixHeap);
    EXPECT_EQ(pol.choose(sample_profile(), 1'000).kind, QueueKind::kPooledBucket);
    std::remove(path.c_str());
}

TEST(QueuePolicy, AFeatureListFromADifferentVersionIsRefused) {
    const auto path = write_temp("lpq_policy_badfeat.lpqpolicy",
                                 "lpqpolicy 1\nfeatures a b c d e f g\n");
    EXPECT_THROW((void)QueueSelectionPolicy::load(path), InvalidConfigError);
    std::remove(path.c_str());
}

TEST(QueuePolicy, MalformedFilesAreRefusedNotGuessedAt) {
    const auto too_few = write_temp(
        "lpq_policy_few.lpqpolicy",
        std::string("lpqpolicy 1\n") + kFeatureLine + "candidate pooled w=1 intercept 0 coef 0 0 0\n");
    EXPECT_THROW((void)QueueSelectionPolicy::load(too_few), InvalidConfigError);
    const auto no_header = write_temp("lpq_policy_nohdr.lpqpolicy", std::string(kFeatureLine));
    EXPECT_THROW((void)QueueSelectionPolicy::load(no_header), InvalidConfigError);
    const auto bad_version = write_temp("lpq_policy_ver.lpqpolicy", "lpqpolicy 9\n");
    EXPECT_THROW((void)QueueSelectionPolicy::load(bad_version), InvalidConfigError);
    const auto unknown = write_temp("lpq_policy_unk.lpqpolicy",
                                    std::string("lpqpolicy 1\n") + kFeatureLine + "mystery 3\n");
    EXPECT_THROW((void)QueueSelectionPolicy::load(unknown), InvalidConfigError);
    EXPECT_THROW((void)QueueSelectionPolicy::load("/nonexistent/dir/x.lpqpolicy"), InvalidConfigError);
    for (const auto& f : {too_few, no_header, bad_version, unknown}) {
        std::remove(f.c_str());
    }
}

TEST(QueuePolicy, ProfileCacheRoundTripsAndRefusesAForeignGraph) {
    Rng rng(5500001);
    const Graph g = weighted_grid(rng, 12, 12, 1, 40, false);
    const GraphMetadata meta = compute_metadata(g);
    const auto cache = (std::filesystem::temp_directory_path() / "lpq_profile_cache.lpqprofile").string();
    std::remove(cache.c_str());

    ProfileSource src = ProfileSource::kCached;
    const WeightProfile first = load_or_compute_profile(cache, 111ULL, g, meta, src);
    EXPECT_EQ(src, ProfileSource::kComputedAndCached);
    EXPECT_EQ(first.nodes, meta.node_count);
    EXPECT_EQ(first.max_weight, meta.max_weight);

    const WeightProfile second = load_or_compute_profile(cache, 111ULL, g, meta, src);
    EXPECT_EQ(src, ProfileSource::kCached);
    EXPECT_EQ(second.p50, first.p50);
    EXPECT_EQ(second.p99, first.p99);
    EXPECT_EQ(second.compute_ns, first.compute_ns);  // the ORIGINAL cost is what is reported

    // A cache written for another graph (different checksum) must not be used.
    const WeightProfile third = load_or_compute_profile(cache, 222ULL, g, meta, src);
    EXPECT_NE(src, ProfileSource::kCached);
    (void)third;
    std::remove(cache.c_str());
}

TEST(QueuePolicy, QuantilesAreOrderedAndWithinTheWeightRange) {
    Rng rng(5500002);
    const Graph g = weighted_grid(rng, 30, 30, 3, 900, false);
    const GraphMetadata meta = compute_metadata(g);
    ProfileSource src;
    const WeightProfile p = load_or_compute_profile("", 0, g, meta, src);
    EXPECT_EQ(src, ProfileSource::kComputed);
    EXPECT_LE(p.min_weight, p.p50);
    EXPECT_LE(p.p50, p.p90);
    EXPECT_LE(p.p90, p.p99);
    EXPECT_LE(p.p99, p.max_weight);
}

}  // namespace
}  // namespace lpq
