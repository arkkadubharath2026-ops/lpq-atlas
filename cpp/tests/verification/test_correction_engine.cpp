// =============================================================================
// Correction engine (ESD.md sections 10 and 16, PRD.md section 13).
//
// The whole failure path is driven here with a closure over a plain vector --
// no graph, no search, no queue. That is the payoff of passing a refill
// function instead of giving the engine a queue reference.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/verification/correction_engine.hpp"

namespace lpq {
namespace {

Candidate cand(NodeId node, double key, float score) {
    Candidate c;
    c.node  = node;
    c.key   = key;
    c.score = score;
    return c;
}

/// A stand-in for the authoritative substrate: a fixed set of entries the
/// refill function draws from, best-scoring first.
///
/// Mirrors what the LPQ will do -- offer the highest-scoring entries up to the
/// window's target and report a bound on the rest -- without any queue.
class FakeQueue {
public:
    explicit FakeQueue(std::vector<Candidate> entries) : entries_(std::move(entries)) {
        std::sort(entries_.begin(), entries_.end(), CandidateBetter{});
    }

    /// Fills the manager up to its current target and returns the exact bound
    /// on whatever has still not been offered.
    ///
    /// The cursor persists across refills within one extraction, so a refill
    /// after an expansion continues from where the previous one stopped and
    /// never re-offers a node. That is the contract CandidateManager::offer
    /// requires, and it is what a real substrate does naturally -- its
    /// enumeration resumes rather than restarting.
    QueueBound refill(CandidateManager& manager) {
        ++refills_;

        while (cursor_ < entries_.size() && cursor_ < manager.target_window()) {
            (void)manager.offer(entries_[cursor_]);
            ++cursor_;
        }

        if (cursor_ >= entries_.size()) {
            return QueueBound::whole_queue();
        }

        // The tightest valid lower bound on the entries not yet offered.
        double lowest = entries_[cursor_].key;
        for (std::size_t i = cursor_; i < entries_.size(); ++i) {
            lowest = std::min(lowest, entries_[i].key);
        }
        return QueueBound::at_least(lowest);
    }

    /// Starts a new extraction: the enumeration restarts from the top.
    void begin_extraction() noexcept { cursor_ = 0; }

    /// The smallest key anywhere in the substrate.
    ///
    /// This, not the node id, is the right ground truth when keys can tie.
    /// Several nodes may hold the minimum key, and any of them is a correct
    /// extraction; which one is chosen depends on which happened to be inside
    /// the window, so asserting on identity would fail the engine for being
    /// right in a different permissible way.
    [[nodiscard]] double true_minimum_key() const {
        double lowest = entries_.front().key;
        for (const Candidate& c : entries_) {
            lowest = std::min(lowest, c.key);
        }
        return lowest;
    }

    /// The minimum node, for the hand-built cases where all keys are distinct
    /// and identity is therefore unambiguous.
    [[nodiscard]] NodeId true_minimum() const {
        const Candidate* best = &entries_.front();
        for (const Candidate& c : entries_) {
            if (c.key < best->key || (c.key == best->key && c.node < best->node)) {
                best = &c;
            }
        }
        return best->node;
    }

    [[nodiscard]] std::uint32_t refills() const noexcept { return refills_; }

private:
    std::vector<Candidate> entries_;
    std::size_t            cursor_  = 0;
    std::uint32_t          refills_ = 0;
};

CandidateWindowConfig window(std::uint32_t k_min, std::uint32_t k_max,
                             std::uint32_t max_expansions = 3) {
    CandidateWindowConfig c;
    c.k_min            = k_min;
    c.k_max            = k_max;
    c.expansion_factor = 2;
    c.max_expansions   = max_expansions;
    return c;
}

/// Opens a window, fills it once, and resolves it.
CorrectionOutcome run(FakeQueue& queue, CandidateManager& manager, VerificationEngine& verifier,
                      CorrectionEngine& corrector, std::uint64_t queue_size) {
    manager.begin_extraction(queue_size);
    queue.begin_extraction();
    const QueueBound bound = queue.refill(manager);
    return corrector.resolve(manager, verifier, bound,
                             [&queue](CandidateManager& m) { return queue.refill(m); });
}

// -----------------------------------------------------------------------------
// The fast path
// -----------------------------------------------------------------------------

TEST(CorrectionEngine, APerfectPredictionResolvesOnTheFirstAttempt) {
    // Scores rank the entries exactly as the keys do.
    FakeQueue queue({cand(1, 10.0, 0.9F), cand(2, 20.0, 0.5F), cand(3, 30.0, 0.1F)});

    CandidateManager   manager{window(2, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 3);

    EXPECT_EQ(o.path, ResolutionPath::kFirstAttempt);
    EXPECT_TRUE(o.resolved());
    EXPECT_FALSE(o.was_expensive());
    EXPECT_EQ(o.attempts, 1U);
    EXPECT_EQ(o.expansions, 0U);
    EXPECT_EQ(o.verification.node, 1U);
    EXPECT_EQ(queue.refills(), 1U);  // no re-fill needed
}

TEST(CorrectionEngine, AMisrankedWindowStillResolvesCheaplyIfTheMinimumIsInIt) {
    // The model's top pick is wrong, but the true minimum is inside the window,
    // so the ordering is repaired with no extra queue work at all.
    FakeQueue queue({cand(1, 90.0, 0.99F), cand(2, 10.0, 0.98F), cand(3, 95.0, 0.01F)});

    CandidateManager   manager{window(2, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 3);

    EXPECT_EQ(o.path, ResolutionPath::kFirstAttemptCorrected);
    EXPECT_TRUE(o.resolved());
    EXPECT_FALSE(o.was_expensive());
    EXPECT_EQ(o.expansions, 0U);
    EXPECT_EQ(o.verification.node, 2U);
    EXPECT_FALSE(o.verification.prediction_was_correct);
    EXPECT_EQ(o.verification.rank_displacement, 1U);
}

// -----------------------------------------------------------------------------
// Expansion (ESD.md section 16)
// -----------------------------------------------------------------------------

TEST(CorrectionEngine, ExpandsTheWindowWhenTheMinimumIsOutsideIt) {
    // The true minimum (node 9) scores worst of all, so a window of two cannot
    // contain it and verification must fail until the window widens.
    FakeQueue queue({cand(1, 50.0, 0.99F), cand(2, 60.0, 0.90F), cand(3, 70.0, 0.80F),
                     cand(4, 80.0, 0.70F), cand(9, 5.0, 0.01F)});

    CandidateManager   manager{window(2, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 5);

    EXPECT_EQ(o.path, ResolutionPath::kExpanded);
    EXPECT_TRUE(o.resolved());
    EXPECT_TRUE(o.was_expensive());
    EXPECT_GT(o.expansions, 0U);
    EXPECT_GT(o.attempts, 1U);
    EXPECT_EQ(o.verification.node, 9U);
    EXPECT_EQ(o.verification.node, queue.true_minimum());
}

TEST(CorrectionEngine, RefillsTheWindowAfterEachExpansion) {
    // Widening without refilling would leave the window unchanged and the retry
    // would fail identically, looping until the budget ran out.
    FakeQueue queue({cand(1, 50.0, 0.99F), cand(2, 60.0, 0.90F), cand(3, 70.0, 0.80F),
                     cand(9, 5.0, 0.01F)});

    CandidateManager   manager{window(1, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 4);

    ASSERT_TRUE(o.resolved());
    EXPECT_EQ(queue.refills(), 1U + o.expansions);
}

// -----------------------------------------------------------------------------
// Fallback
// -----------------------------------------------------------------------------

TEST(CorrectionEngine, FallsBackWhenTheWindowCannotGrowFarEnough) {
    // k_max is 2, so the window can never reach the badly scored minimum.
    FakeQueue queue({cand(1, 50.0, 0.99F), cand(2, 60.0, 0.90F), cand(3, 70.0, 0.80F),
                     cand(9, 5.0, 0.01F)});

    CandidateManager   manager{window(2, 2)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 4);

    EXPECT_EQ(o.path, ResolutionPath::kFallback);
    EXPECT_FALSE(o.resolved());
    EXPECT_TRUE(o.was_expensive());
    EXPECT_EQ(corrector.metrics().fallbacks, 1U);
}

TEST(CorrectionEngine, FallsBackWhenTheExpansionBudgetIsSpent) {
    FakeQueue queue({cand(1, 50.0, 0.99F), cand(2, 60.0, 0.90F), cand(3, 70.0, 0.80F),
                     cand(4, 75.0, 0.70F), cand(5, 78.0, 0.60F), cand(6, 79.0, 0.50F),
                     cand(9, 5.0, 0.01F)});

    CandidateManager   manager{window(1, 64, /*max_expansions=*/1)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 7);

    EXPECT_EQ(o.path, ResolutionPath::kFallback);
    EXPECT_EQ(o.expansions, 1U);
}

// Budget exhaustion is terminal: unlike a narrow window it cannot be repaired
// by looking at more candidates, because the cost of looking is what ran out.
TEST(CorrectionEngine, FallsBackImmediatelyWhenTheVerificationBudgetIsSpent) {
    CorrectnessPolicy policy;
    policy.verification_budget = 3;

    FakeQueue queue({cand(1, 50.0, 0.99F), cand(2, 60.0, 0.90F), cand(3, 70.0, 0.80F),
                     cand(4, 75.0, 0.70F), cand(5, 78.0, 0.60F), cand(9, 5.0, 0.01F)});

    CandidateManager   manager{window(4, 64, /*max_expansions=*/8)};
    VerificationEngine verifier(policy);
    CorrectionEngine   corrector;

    const CorrectionOutcome o = run(queue, manager, verifier, corrector, 6);

    EXPECT_EQ(o.path, ResolutionPath::kFallback);
    EXPECT_GT(verifier.metrics().budget_exhausted, 0U);
}

TEST(CorrectionEngine, ReportsAnEmptyWindowWithoutAttemptingAnything) {
    CandidateManager   manager{window(2, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    manager.begin_extraction(0);
    const CorrectionOutcome o =
        corrector.resolve(manager, verifier, QueueBound::whole_queue(),
                          [](CandidateManager&) { return QueueBound::whole_queue(); });

    EXPECT_EQ(o.path, ResolutionPath::kEmpty);
    EXPECT_FALSE(o.resolved());
    EXPECT_EQ(o.attempts, 0U);
    EXPECT_EQ(corrector.metrics().empty, 1U);
}

// -----------------------------------------------------------------------------
// Termination and correctness under adversarial scoring
// -----------------------------------------------------------------------------

// The loop must terminate on every input. A correction loop that could spin is
// far worse than one that gives up, because it would present as a hang in the
// middle of a benchmark rather than as a measurable fallback.
TEST(CorrectionEngine, AlwaysTerminatesAndNeverExceedsItsBudgets) {
    Rng rng(20260922U);

    for (int trial = 0; trial < 3000; ++trial) {
        const std::size_t n = 1 + static_cast<std::size_t>(rng.next_below(30));

        std::vector<Candidate> entries;
        for (std::size_t i = 0; i < n; ++i) {
            // Scores are uncorrelated with keys, and keys are drawn from a
            // small range so exact ties are frequent.
            entries.push_back(cand(static_cast<NodeId>(i),
                                   static_cast<double>(rng.next_below(10)),
                                   static_cast<float>(rng.next_double(0.0, 1.0))));
        }
        FakeQueue queue(entries);

        const std::uint32_t k_min = 1 + static_cast<std::uint32_t>(rng.next_below(4));
        const std::uint32_t k_max = k_min + static_cast<std::uint32_t>(rng.next_below(8));
        const std::uint32_t budget = 1 + static_cast<std::uint32_t>(rng.next_below(4));

        CandidateManager   manager{window(k_min, k_max, budget)};
        VerificationEngine verifier;
        CorrectionEngine   corrector;

        const CorrectionOutcome o = run(queue, manager, verifier, corrector, n);

        EXPECT_LE(o.expansions, budget) << "trial " << trial;
        EXPECT_LE(o.attempts, budget + 1) << "trial " << trial;

        // Whenever it resolved, it resolved CORRECTLY. A correction loop that
        // returned a non-minimal node would be worse than one that fell back.
        if (o.resolved()) {
            EXPECT_EQ(o.verification.key, queue.true_minimum_key()) << "trial " << trial;
        }
    }
}

// The complement to the sweep above: if every trial resolved, the fallback path
// would never be exercised and its correctness would be untested.
TEST(CorrectionEngine, BothResolutionAndFallbackOccurUnderAdversarialScoring) {
    Rng rng(1002U);  // the differential regression seed

    std::uint64_t resolved  = 0;
    std::uint64_t fell_back = 0;

    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<Candidate> entries;
        for (NodeId u = 0; u < 20; ++u) {
            entries.push_back(cand(u, rng.next_double(0.0, 100.0),
                                   static_cast<float>(rng.next_double(0.0, 1.0))));
        }
        FakeQueue queue(entries);

        CandidateManager   manager{window(1, 4, 1)};  // deliberately too tight
        VerificationEngine verifier;
        CorrectionEngine   corrector;

        const CorrectionOutcome o = run(queue, manager, verifier, corrector, 20);
        if (o.resolved()) {
            ++resolved;
            EXPECT_EQ(o.verification.key, queue.true_minimum_key()) << "trial " << trial;
        } else {
            ++fell_back;
        }
    }

    EXPECT_GT(resolved, 0U);
    EXPECT_GT(fell_back, 0U);
}

// -----------------------------------------------------------------------------
// Metrics (PRD.md section 13)
// -----------------------------------------------------------------------------

TEST(CorrectionMetrics, SeparateTheFastPathFromCorrectionsAndFallbacks) {
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    {  // fast path
        FakeQueue        queue({cand(1, 10.0, 0.9F), cand(2, 20.0, 0.1F)});
        CandidateManager manager{window(2, 8)};
        (void)run(queue, manager, verifier, corrector, 2);
    }
    {  // corrected in place
        FakeQueue        queue({cand(1, 90.0, 0.9F), cand(2, 10.0, 0.8F)});
        CandidateManager manager{window(2, 8)};
        (void)run(queue, manager, verifier, corrector, 2);
    }
    {  // fallback
        FakeQueue        queue({cand(1, 90.0, 0.9F), cand(2, 91.0, 0.8F), cand(9, 1.0, 0.01F)});
        CandidateManager manager{window(2, 2)};
        (void)run(queue, manager, verifier, corrector, 3);
    }

    const CorrectionMetrics& m = corrector.metrics();
    EXPECT_EQ(m.extractions, 3U);
    EXPECT_EQ(m.first_attempt, 1U);
    EXPECT_EQ(m.first_attempt_corrected, 1U);
    EXPECT_EQ(m.fallbacks, 1U);
    EXPECT_NEAR(m.fast_path_rate(), 1.0 / 3.0, 1e-9);
    EXPECT_NEAR(m.fallback_rate(), 1.0 / 3.0, 1e-9);
}

// The two accuracy figures answer different questions and must not be confused.
// The verification engine's is conditioned on a verification having succeeded;
// the correction engine's counts fallbacks against the model, because a
// fallback is a prediction that cost time and delivered nothing.
TEST(CorrectionMetrics, FastPathRateCountsFallbacksAgainstTheModelButAccuracyDoesNot) {
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    {  // resolves on the fast path
        FakeQueue        queue({cand(1, 10.0, 0.9F), cand(2, 20.0, 0.1F)});
        CandidateManager manager{window(2, 8)};
        (void)run(queue, manager, verifier, corrector, 2);
    }
    {  // cannot resolve at all
        FakeQueue        queue({cand(1, 90.0, 0.9F), cand(2, 91.0, 0.8F), cand(9, 1.0, 0.01F)});
        CandidateManager manager{window(2, 2)};
        (void)run(queue, manager, verifier, corrector, 3);
    }

    // One acceptance, and the model's top pick was right in it.
    EXPECT_NEAR(verifier.metrics().prediction_accuracy(), 1.0, 1e-12);
    // But only half of the extractions took the ideal path.
    EXPECT_NEAR(corrector.metrics().fast_path_rate(), 0.5, 1e-12);
}

TEST(CorrectionMetrics, ResetClearsEverything) {
    FakeQueue          queue({cand(1, 10.0, 0.9F)});
    CandidateManager   manager{window(2, 8)};
    VerificationEngine verifier;
    CorrectionEngine   corrector;

    (void)run(queue, manager, verifier, corrector, 1);
    ASSERT_GT(corrector.metrics().extractions, 0U);

    corrector.reset_metrics();
    EXPECT_EQ(corrector.metrics().extractions, 0U);
    EXPECT_NEAR(corrector.metrics().fast_path_rate(), 0.0, 1e-12);
    EXPECT_NEAR(corrector.metrics().mean_attempts_per_extraction(), 0.0, 1e-12);
}

}  // namespace
}  // namespace lpq
