// =============================================================================
// Candidate manager (ESD.md sections 10 and 15, PRD.md section 13).
//
// The window policy is tested here WITHOUT a graph, a search or a queue, which
// is the payoff of keeping the component free of queue internals.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/lpq/candidate_manager.hpp"

namespace lpq {
namespace {

Candidate make(NodeId node, float score, double key = 0.0, float confidence = 0.5F) {
    Candidate c;
    c.node       = node;
    c.score      = score;
    c.key        = key;
    c.confidence = confidence;
    return c;
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

TEST(CandidateWindowConfig, AcceptsTheDefaults) {
    EXPECT_NO_THROW(CandidateWindowConfig{}.validate());
}

TEST(CandidateWindowConfig, RejectsAWindowThatCouldNotBound) {
    CandidateWindowConfig c;

    c       = CandidateWindowConfig{};
    c.k_min = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c       = CandidateWindowConfig{};
    c.k_min = 10;
    c.k_max = 4;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c       = CandidateWindowConfig{};
    c.alpha = 0.0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c       = CandidateWindowConfig{};
    c.alpha = -1.0;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

TEST(CandidateWindowConfig, RejectsAnExpansionFactorThatWouldNeverMakeProgress) {
    // A factor of 1 re-runs verification on an identical window forever.
    CandidateWindowConfig c;
    c.expansion_factor = 1;
    EXPECT_THROW(c.validate(), InvalidConfigError);
    c.expansion_factor = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Window sizing (ESD.md section 15)
// -----------------------------------------------------------------------------

TEST(WindowSize, FollowsTheSpecifiedFormula) {
    CandidateWindowConfig config;
    config.k_min = 1;
    config.k_max = 1000;
    config.alpha = 1.0;

    // k = ceil(log2(queue_size + 1)), clamped.
    EXPECT_EQ(window_size_for(1, config), 1U);   // log2(2) = 1
    EXPECT_EQ(window_size_for(2, config), 2U);   // log2(3) = 1.58 -> 2
    EXPECT_EQ(window_size_for(3, config), 2U);   // log2(4) = 2
    EXPECT_EQ(window_size_for(4, config), 3U);   // log2(5) = 2.32 -> 3
    EXPECT_EQ(window_size_for(7, config), 3U);   // log2(8) = 3
    EXPECT_EQ(window_size_for(8, config), 4U);   // log2(9) = 3.17 -> 4
    EXPECT_EQ(window_size_for(1023, config), 10U);
    EXPECT_EQ(window_size_for(1024, config), 11U);
}

// The integer fast path is taken whenever alpha == 1, so its equivalence to the
// specified formula is load-bearing rather than incidental.
//
// The reference is computed here in floating point directly, rather than by
// perturbing alpha to force the slow branch inside window_size_for. An alpha of
// 1 + epsilon is NOT equivalent to alpha == 1: at every exact power of two,
// log2(n + 1) is a whole number, and multiplying it by anything above 1 pushes
// ceil() to the next integer. That difference is a genuine property of the
// formula, not a bug in either path.
TEST(WindowSize, TheIntegerFastPathAgreesWithTheSpecifiedFormula) {
    CandidateWindowConfig config;
    config.k_min = 1;
    config.k_max = 4000000000U;
    config.alpha = 1.0;

    auto reference = [&config](std::uint64_t n) {
        const double k = std::ceil(std::log2(static_cast<double>(n) + 1.0));
        return static_cast<std::uint32_t>(std::max<double>(k, config.k_min));
    };

    for (std::uint64_t n : {1ULL, 2ULL, 3ULL, 5ULL, 7ULL, 8ULL, 9ULL, 15ULL, 16ULL, 17ULL,
                            1000ULL, 65535ULL, 65536ULL, 1000000ULL, 16777216ULL}) {
        EXPECT_EQ(window_size_for(n, config), reference(n)) << "queue size " << n;
    }

    // And across a dense sweep of small sizes, where off-by-one errors live --
    // in particular at and around every power of two.
    for (std::uint64_t n = 1; n < 20000; ++n) {
        EXPECT_EQ(window_size_for(n, config), reference(n)) << "queue size " << n;
    }
}

// The complement: with alpha != 1 the floating-point branch runs, and it must
// still satisfy the formula.
TEST(WindowSize, TheFloatingPointBranchFollowsTheFormula) {
    for (double alpha : {0.5, 1.5, 2.0, 3.25}) {
        CandidateWindowConfig config;
        config.k_min = 1;
        config.k_max = 4000000000U;
        config.alpha = alpha;

        for (std::uint64_t n = 1; n < 3000; ++n) {
            const double expected =
                std::ceil(alpha * std::log2(static_cast<double>(n) + 1.0));
            EXPECT_EQ(window_size_for(n, config),
                      static_cast<std::uint32_t>(std::max(expected, 1.0)))
                << "alpha " << alpha << ", queue size " << n;
        }
    }
}

TEST(WindowSize, RespectsTheMinimum) {
    CandidateWindowConfig config;
    config.k_min = 8;
    config.k_max = 32;
    EXPECT_EQ(window_size_for(1, config), 8U);
    EXPECT_EQ(window_size_for(4, config), 8U);
}

// ESD.md section 15: windows "must never grow without a configurable maximum".
TEST(WindowSize, NeverExceedsTheMaximumAtAnyQueueSize) {
    CandidateWindowConfig config;
    config.k_min = 2;
    config.k_max = 16;

    for (std::uint64_t n : {1ULL, 100ULL, 1000000ULL, 1ULL << 40, ~0ULL}) {
        EXPECT_LE(window_size_for(n, config), 16U) << "queue size " << n;
    }
}

TEST(WindowSize, AlphaScalesTheWindow) {
    CandidateWindowConfig narrow;
    narrow.k_min = 1;
    narrow.k_max = 1000;
    narrow.alpha = 0.5;

    CandidateWindowConfig wide = narrow;
    wide.alpha                 = 2.0;

    EXPECT_LT(window_size_for(1024, narrow), window_size_for(1024, wide));
    EXPECT_EQ(window_size_for(1023, narrow), 5U);   // ceil(0.5 * 10)
    EXPECT_EQ(window_size_for(1023, wide), 20U);    // ceil(2.0 * 10)
}

TEST(WindowSize, IsMonotoneInQueueSize) {
    // A larger queue must never produce a narrower window; a non-monotone
    // policy would make the search's behaviour discontinuous in ways that are
    // very hard to attribute when reading a benchmark.
    CandidateWindowConfig config;
    config.k_min = 1;
    config.k_max = 100000;

    std::uint32_t previous = 0;
    for (std::uint64_t n = 1; n < 20000; ++n) {
        const std::uint32_t k = window_size_for(n, config);
        EXPECT_GE(k, previous) << "queue size " << n;
        previous = k;
    }
}

// -----------------------------------------------------------------------------
// Admission
// -----------------------------------------------------------------------------

CandidateWindowConfig fixed_window(std::uint32_t k) {
    CandidateWindowConfig c;
    c.k_min = k;
    c.k_max = k;
    return c;
}

TEST(CandidateManager, StartsEmpty) {
    CandidateManager m;
    m.begin_extraction(100);
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0U);
    EXPECT_FALSE(m.full());
}

TEST(CandidateManager, KeepsCandidatesOrderedBestFirst) {
    CandidateManager m{fixed_window(4)};
    m.begin_extraction(16);

    EXPECT_TRUE(m.offer(make(1, 0.2F)));
    EXPECT_TRUE(m.offer(make(2, 0.9F)));
    EXPECT_TRUE(m.offer(make(3, 0.5F)));

    const auto window = m.candidates();
    ASSERT_EQ(window.size(), 3U);
    EXPECT_EQ(window[0].node, 2U);
    EXPECT_EQ(window[1].node, 3U);
    EXPECT_EQ(window[2].node, 1U);
    EXPECT_EQ(m.best().node, 2U);
}

TEST(CandidateManager, RejectsACandidateThatDoesNotFit) {
    CandidateManager m{fixed_window(2)};
    m.begin_extraction(16);

    EXPECT_TRUE(m.offer(make(1, 0.9F)));
    EXPECT_TRUE(m.offer(make(2, 0.8F)));
    EXPECT_TRUE(m.full());

    EXPECT_FALSE(m.offer(make(3, 0.1F)));
    EXPECT_EQ(m.size(), 2U);
    EXPECT_EQ(m.metrics().candidates_rejected, 1U);
}

TEST(CandidateManager, EvictsTheWorstWhenABetterCandidateArrives) {
    CandidateManager m{fixed_window(2)};
    m.begin_extraction(16);

    m.offer(make(1, 0.5F));
    m.offer(make(2, 0.4F));
    EXPECT_TRUE(m.offer(make(3, 0.9F)));

    ASSERT_EQ(m.size(), 2U);
    EXPECT_EQ(m.candidates()[0].node, 3U);
    EXPECT_EQ(m.candidates()[1].node, 1U);
}

TEST(CandidateManager, NeverExceedsItsTargetWindow) {
    CandidateManager m{fixed_window(5)};
    Rng              rng(1234U);

    m.begin_extraction(1000);
    for (NodeId u = 0; u < 500; ++u) {
        m.offer(make(u, static_cast<float>(rng.next_double(0.0, 1.0))));
        ASSERT_LE(m.size(), 5U) << "after offering node " << u;
    }
}

// The central correctness property: whatever the offer order, the window holds
// exactly the top-k a full sort would select.
TEST(CandidateManager, HoldsExactlyTheTopKAcrossRandomOfferOrders) {
    Rng rng(20260922U);

    for (int trial = 0; trial < 500; ++trial) {
        const std::uint32_t k = 1 + static_cast<std::uint32_t>(rng.next_below(12));
        const std::size_t   n = 1 + static_cast<std::size_t>(rng.next_below(60));

        std::vector<Candidate> all;
        for (std::size_t i = 0; i < n; ++i) {
            // Scores drawn from a small set so ties are common: the tie-break
            // is where an ordering bug would hide.
            all.push_back(make(static_cast<NodeId>(i),
                               static_cast<float>(rng.next_below(8)) * 0.125F));
        }

        CandidateManager m{fixed_window(k)};
        m.begin_extraction(64);
        for (const Candidate& c : all) {
            m.offer(c);
        }

        std::vector<Candidate> expected = all;
        std::sort(expected.begin(), expected.end(), CandidateBetter{});
        expected.resize(std::min<std::size_t>(k, expected.size()));

        const auto window = m.candidates();
        ASSERT_EQ(window.size(), expected.size()) << "trial " << trial;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(window[i].node, expected[i].node)
                << "trial " << trial << ", position " << i;
        }
    }
}

TEST(CandidateManager, ResultDoesNotDependOnOfferOrder) {
    // Same candidates, opposite order. A window that differed would mean the
    // search path depended on adjacency enumeration rather than on the seed.
    std::vector<Candidate> forward;
    for (NodeId u = 0; u < 20; ++u) {
        forward.push_back(make(u, static_cast<float>(u % 4) * 0.25F));
    }
    std::vector<Candidate> backward(forward.rbegin(), forward.rend());

    CandidateManager a{fixed_window(6)};
    CandidateManager b{fixed_window(6)};
    a.begin_extraction(64);
    b.begin_extraction(64);
    for (const Candidate& c : forward) {
        a.offer(c);
    }
    for (const Candidate& c : backward) {
        b.offer(c);
    }

    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a.candidates()[i].node, b.candidates()[i].node) << "position " << i;
    }
}

TEST(CandidateManager, BeginExtractionClearsThePreviousWindow) {
    CandidateManager m{fixed_window(4)};

    m.begin_extraction(16);
    m.offer(make(1, 0.5F));
    ASSERT_EQ(m.size(), 1U);

    m.begin_extraction(16);
    EXPECT_TRUE(m.empty());
}

// -----------------------------------------------------------------------------
// Prediction versus reality
// -----------------------------------------------------------------------------

// best() is what the model believes; min_key_candidate() is what the keys say.
// The gap between them is exactly what verification exists to detect.
TEST(CandidateManager, DistinguishesTheBestPredictionFromTheSmallestKey) {
    CandidateManager m{fixed_window(4)};
    m.begin_extraction(16);

    m.offer(make(1, /*score=*/0.9F, /*key=*/50.0));  // model's favourite
    m.offer(make(2, /*score=*/0.5F, /*key=*/10.0));  // actually the minimum
    m.offer(make(3, /*score=*/0.1F, /*key=*/99.0));

    EXPECT_EQ(m.best().node, 1U);
    EXPECT_EQ(m.min_key_candidate().node, 2U);
}

TEST(CandidateManager, AgreesWithItselfWhenThePredictionIsCorrect) {
    CandidateManager m{fixed_window(4)};
    m.begin_extraction(16);

    m.offer(make(1, 0.9F, 10.0));
    m.offer(make(2, 0.5F, 20.0));

    EXPECT_EQ(m.best().node, m.min_key_candidate().node);
}

TEST(CandidateManager, BreaksKeyTiesDeterministically) {
    CandidateManager forward{fixed_window(4)};
    CandidateManager backward{fixed_window(4)};
    forward.begin_extraction(16);
    backward.begin_extraction(16);

    forward.offer(make(7, 0.9F, 5.0));
    forward.offer(make(3, 0.8F, 5.0));
    backward.offer(make(3, 0.8F, 5.0));
    backward.offer(make(7, 0.9F, 5.0));

    EXPECT_EQ(forward.min_key_candidate().node, 3U);
    EXPECT_EQ(backward.min_key_candidate().node, 3U);
}

TEST(CandidateManager, ReportsTheRankOfANode) {
    CandidateManager m{fixed_window(4)};
    m.begin_extraction(16);
    m.offer(make(10, 0.9F));
    m.offer(make(20, 0.5F));
    m.offer(make(30, 0.1F));

    EXPECT_EQ(m.rank_of(10), 0U);
    EXPECT_EQ(m.rank_of(20), 1U);
    EXPECT_EQ(m.rank_of(30), 2U);
    EXPECT_EQ(m.rank_of(99), m.size());  // absent
}

// -----------------------------------------------------------------------------
// Expansion (ESD.md section 16)
// -----------------------------------------------------------------------------

TEST(CandidateManager, ExpansionWidensTheWindowByTheConfiguredFactor) {
    CandidateWindowConfig config;
    config.k_min            = 4;
    config.k_max            = 64;
    config.expansion_factor = 2;
    config.max_expansions   = 3;

    CandidateManager m(config);
    m.begin_extraction(1);  // clamps to k_min
    EXPECT_EQ(m.target_window(), 4U);

    EXPECT_TRUE(m.expand());
    EXPECT_EQ(m.target_window(), 8U);
    EXPECT_TRUE(m.expand());
    EXPECT_EQ(m.target_window(), 16U);
}

TEST(CandidateManager, ExpansionKeepsTheCandidatesAlreadyAdmitted) {
    // Re-offering them would be wasted work: they were scored against the same
    // predictions.
    CandidateWindowConfig config = fixed_window(2);
    config.k_max                 = 8;

    CandidateManager m(config);
    m.begin_extraction(1);
    m.offer(make(1, 0.9F));
    m.offer(make(2, 0.8F));

    ASSERT_TRUE(m.expand());
    ASSERT_EQ(m.size(), 2U);
    EXPECT_EQ(m.candidates()[0].node, 1U);
    EXPECT_TRUE(m.offer(make(3, 0.7F)));
    EXPECT_EQ(m.size(), 3U);
}

TEST(CandidateManager, ExpansionStopsAtTheConfiguredMaximumWindow) {
    CandidateWindowConfig config;
    config.k_min            = 4;
    config.k_max            = 10;
    config.expansion_factor = 4;
    config.max_expansions   = 10;

    CandidateManager m(config);
    m.begin_extraction(1);
    EXPECT_TRUE(m.expand());
    EXPECT_EQ(m.target_window(), 10U);  // 16 clamped to 10
    EXPECT_FALSE(m.expand());           // already at the ceiling
    EXPECT_FALSE(m.can_expand());
}

TEST(CandidateManager, ExpansionStopsAtTheExpansionBudget) {
    // The budget bounds an extraction's total work. Without it, a search in
    // which every extraction widens to k_max does strictly more work than the
    // classical queue and shows up as an unexplained slowdown.
    CandidateWindowConfig config;
    config.k_min            = 2;
    config.k_max            = 1024;
    config.expansion_factor = 2;
    config.max_expansions   = 2;

    CandidateManager m(config);
    m.begin_extraction(1);

    EXPECT_TRUE(m.expand());
    EXPECT_TRUE(m.expand());
    EXPECT_FALSE(m.expand());
    EXPECT_EQ(m.expansions_this_extraction(), 2U);
    EXPECT_EQ(m.metrics().expansions_exhausted, 1U);
}

TEST(CandidateManager, ExpansionBudgetResetsWithEachExtraction) {
    CandidateWindowConfig config;
    config.k_min          = 2;
    config.k_max          = 64;
    config.max_expansions = 1;

    CandidateManager m(config);

    m.begin_extraction(1);
    EXPECT_TRUE(m.expand());
    EXPECT_FALSE(m.expand());

    m.begin_extraction(1);
    EXPECT_EQ(m.expansions_this_extraction(), 0U);
    EXPECT_TRUE(m.expand());
}

TEST(CandidateManager, ExpansionNeverOverflowsTheWindowBound) {
    // A large factor against a large target must not wrap the multiplication.
    CandidateWindowConfig config;
    config.k_min            = 3000U;
    config.k_max            = kMaxSupportedWindow;
    config.expansion_factor = 1000000U;
    config.max_expansions   = 5;

    CandidateManager m(config);
    m.begin_extraction(1000000);
    EXPECT_TRUE(m.expand());
    EXPECT_EQ(m.target_window(), kMaxSupportedWindow);
}

TEST(CandidateWindowConfig, RejectsAWindowLargeEnoughToExhaustMemory) {
    // The window is allocated once at k_max, so an unbounded k_max would try to
    // reserve tens of gigabytes before the first query ran -- and a window that
    // large is a linear scan of the queue rather than a candidate window.
    CandidateWindowConfig c;
    c.k_max = kMaxSupportedWindow + 1;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c.k_max = 2000000000U;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c.k_max = kMaxSupportedWindow;
    EXPECT_NO_THROW(c.validate());
}

// -----------------------------------------------------------------------------
// Metrics (PRD.md section 13)
// -----------------------------------------------------------------------------

TEST(CandidateMetrics, CountOffersAdmissionsAndRejections) {
    CandidateManager m{fixed_window(2)};
    m.begin_extraction(16);

    m.offer(make(1, 0.9F));
    m.offer(make(2, 0.8F));
    m.offer(make(3, 0.1F));  // rejected

    const CandidateMetrics& metrics = m.metrics();
    EXPECT_EQ(metrics.extractions, 1U);
    EXPECT_EQ(metrics.candidates_offered, 3U);
    EXPECT_EQ(metrics.candidates_admitted, 2U);
    EXPECT_EQ(metrics.candidates_rejected, 1U);
    EXPECT_NEAR(metrics.rejection_rate(), 1.0 / 3.0, 1e-9);
}

TEST(CandidateMetrics, AnEvictionCountsAsAnAdmissionNotARejection) {
    // The evicted candidate was admitted earlier and later displaced; counting
    // the displacing offer as a rejection would understate admissions and make
    // the rejection rate unreadable.
    CandidateManager m{fixed_window(1)};
    m.begin_extraction(16);
    m.offer(make(1, 0.1F));
    m.offer(make(2, 0.9F));

    EXPECT_EQ(m.metrics().candidates_admitted, 2U);
    EXPECT_EQ(m.metrics().candidates_rejected, 0U);
    EXPECT_EQ(m.size(), 1U);
}

TEST(CandidateMetrics, MeanWindowSizeReflectsExpansions) {
    // The metric tracks each extraction's FINAL window. Recording only the
    // opening size would understate exactly the cases that matter, since an
    // extraction only widens when the prediction was wrong.
    CandidateWindowConfig config;
    config.k_min            = 4;
    config.k_max            = 64;
    config.expansion_factor = 2;
    config.max_expansions   = 4;

    CandidateManager m(config);

    m.begin_extraction(1);  // window 4, never expanded
    m.begin_extraction(1);  // window 4 -> 8
    ASSERT_TRUE(m.expand());

    EXPECT_EQ(m.metrics().extractions, 2U);
    EXPECT_EQ(m.metrics().window_size_sum, 12U);  // 4 + 8
    EXPECT_NEAR(m.metrics().mean_window_size(), 6.0, 1e-9);
    EXPECT_EQ(m.metrics().max_window_used, 8U);
}

TEST(CandidateMetrics, AccumulateAcrossExtractionsAndResetOnDemand) {
    CandidateManager m{fixed_window(4)};
    for (int i = 0; i < 10; ++i) {
        m.begin_extraction(16);
        m.offer(make(1, 0.5F));
    }
    EXPECT_EQ(m.metrics().extractions, 10U);
    EXPECT_EQ(m.metrics().candidates_offered, 10U);

    m.reset_metrics();
    EXPECT_EQ(m.metrics().extractions, 0U);
    EXPECT_NEAR(m.metrics().mean_window_size(), 0.0, 1e-12);
    EXPECT_NEAR(m.metrics().rejection_rate(), 0.0, 1e-12);
}

TEST(CandidateManager, DoesNotAllocateDuringASearch) {
    // Storage is reserved once at k_max. An allocation inside the extraction
    // loop would be a latency spike and a source of run-to-run variance in
    // exactly the measurement this project exists to make.
    CandidateManager m{fixed_window(16)};
    m.begin_extraction(64);
    const std::size_t reserved = m.memory_bytes();

    Rng rng(99U);
    for (int extraction = 0; extraction < 2000; ++extraction) {
        m.begin_extraction(64);
        for (NodeId u = 0; u < 40; ++u) {
            m.offer(make(u, static_cast<float>(rng.next_double(0.0, 1.0))));
        }
        EXPECT_EQ(m.memory_bytes(), reserved) << "at extraction " << extraction;
    }
}

// -----------------------------------------------------------------------------
// D-14: `confidence` does not gate the window.
//
// PRD section 2 lists "confidence-aware candidate extraction" among the intended
// contributions. Candidate::confidence exists, but nothing in the window policy
// reads it. This pins the CURRENT behaviour so the claim cannot drift: the
// window's membership, its order and its accounting are functions of
// (node, key, score) alone. It is a statement about what the code does, not a
// verdict on what it should do -- if confidence is ever made to gate
// extraction, this test must be changed deliberately, together with D-14 in
// docs/atlas/DISCREPANCIES.md.
// -----------------------------------------------------------------------------

TEST(CandidateManager, ConfidenceDoesNotChangeTheWindowItsOrderOrItsAccounting) {
    // The same (node, key, score) stream offered with four different
    // confidence assignments: none, certain, inverted against the score, and
    // arbitrary (seeded).
    constexpr std::size_t kOffers = 40;
    const auto confidence_for = [](int variant, float score, Rng& rng) -> float {
        switch (variant) {
            case 0: return 0.0F;
            case 1: return 1.0F;
            case 2: return 1.0F - score;
            default: return static_cast<float>(rng.next_double(0.0, 1.0));
        }
    };

    std::vector<NodeId> reference_order;
    std::uint64_t       reference_rejected = 0;
    for (int variant = 0; variant < 4; ++variant) {
        Rng stream(2026U);  // identical (node, score) stream for every variant
        Rng noise(7U);
        CandidateManager m{fixed_window(8)};
        m.begin_extraction(64);
        for (std::size_t i = 0; i < kOffers; ++i) {
            const float score = static_cast<float>(stream.next_double(0.0, 1.0));
            m.offer(make(static_cast<NodeId>(i), score, static_cast<double>(i),
                         confidence_for(variant, score, noise)));
        }
        std::vector<NodeId> order;
        for (const auto& c : m.candidates()) {
            order.push_back(c.node);
        }
        ASSERT_EQ(order.size(), 8U);
        if (variant == 0) {
            reference_order    = order;
            reference_rejected = m.metrics().candidates_rejected;
        } else {
            EXPECT_EQ(order, reference_order) << "confidence variant " << variant;
            EXPECT_EQ(m.metrics().candidates_rejected, reference_rejected)
                << "confidence variant " << variant;
        }
    }
}

TEST(CandidateManager, ALowConfidenceCandidateStillBeatsAConfidentOneWithALowerScore) {
    CandidateManager m{fixed_window(2)};
    m.begin_extraction(16);
    m.offer(make(/*node=*/1, /*score=*/0.9F, /*key=*/1.0, /*confidence=*/0.0F));
    m.offer(make(/*node=*/2, /*score=*/0.1F, /*key=*/2.0, /*confidence=*/1.0F));
    EXPECT_EQ(m.best().node, 1U);

    // And a certain, low-scoring candidate does not displace a better-scored
    // one when the window is full.
    m.offer(make(/*node=*/3, /*score=*/0.05F, /*key=*/3.0, /*confidence=*/1.0F));
    ASSERT_EQ(m.size(), 2U);
    EXPECT_EQ(m.candidates()[0].node, 1U);
    EXPECT_EQ(m.candidates()[1].node, 2U);
}

}  // namespace
}  // namespace lpq
