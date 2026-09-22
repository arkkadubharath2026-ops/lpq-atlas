// =============================================================================
// Verification engine (ESD.md sections 10, 16 and 17, PRD.md sections 4 and 13).
//
// The centrepiece is VerificationSoundness.AcceptedExtractionIsAlwaysTheTrue
// Minimum, which checks the argument the whole design rests on against a
// brute-force ground truth over thousands of randomly generated queues.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/verification/verification_engine.hpp"

namespace lpq {
namespace {

Candidate cand(NodeId node, double key, float score) {
    Candidate c;
    c.node  = node;
    c.key   = key;
    c.score = score;
    return c;
}

// -----------------------------------------------------------------------------
// Policy (ESD.md section 17)
// -----------------------------------------------------------------------------

TEST(CorrectnessPolicy, ExactIsTheDefaultAndIsFormallyGuaranteed) {
    const CorrectnessPolicy p;
    EXPECT_EQ(p.mode, CorrectnessMode::kExact);
    EXPECT_EQ(p.epsilon, 0.0);
    EXPECT_EQ(p.guarantee(), GuaranteeKind::kFormal);
    EXPECT_EQ(p.slack_factor(), 1.0);
    EXPECT_NO_THROW(p.validate());
}

// ESD.md section 17: "Do not call a heuristic 'guaranteed'." Made executable:
// if bounded-approximate mode ever starts reporting a formal guarantee, this
// fails. The end-to-end path-cost bound is not proven in this codebase, so the
// mode must not claim it is.
TEST(CorrectnessPolicy, ApproximateModeDoesNotClaimAFormalGuarantee) {
    const CorrectnessPolicy p = CorrectnessPolicy::approximate(0.1);
    EXPECT_EQ(p.guarantee(), GuaranteeKind::kEmpirical);
    EXPECT_NE(p.guarantee(), GuaranteeKind::kFormal);
    EXPECT_NEAR(p.slack_factor(), 1.1, 1e-12);

    const std::string statement = guarantee_statement(CorrectnessMode::kBoundedApproximate);
    EXPECT_NE(statement.find("empirical"), std::string::npos);
    EXPECT_NE(statement.find("not "), std::string::npos);
}

TEST(CorrectnessPolicy, RejectsSelfContradictoryConfigurations) {
    CorrectnessPolicy p;

    // Exact mode carrying a slack would report exact results while applying an
    // approximation.
    p         = CorrectnessPolicy{};
    p.epsilon = 0.01;
    EXPECT_THROW(p.validate(), InvalidConfigError);

    // Approximate mode with epsilon 0 is exact mode wearing the wrong label,
    // and would be reported with a weaker guarantee than it actually provides.
    p         = CorrectnessPolicy{};
    p.mode    = CorrectnessMode::kBoundedApproximate;
    p.epsilon = 0.0;
    EXPECT_THROW(p.validate(), InvalidConfigError);

    p         = CorrectnessPolicy{};
    p.mode    = CorrectnessMode::kBoundedApproximate;
    p.epsilon = -0.1;
    EXPECT_THROW(p.validate(), InvalidConfigError);

    p                     = CorrectnessPolicy{};
    p.verification_budget = 0;
    EXPECT_THROW(p.validate(), InvalidConfigError);
}

TEST(CorrectnessPolicy, RejectsANonFiniteEpsilon) {
    CorrectnessPolicy p;
    p.mode    = CorrectnessMode::kBoundedApproximate;
    p.epsilon = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(p.validate(), InvalidConfigError);

    p.epsilon = std::numeric_limits<double>::infinity();
    EXPECT_THROW(p.validate(), InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Exact-mode acceptance
// -----------------------------------------------------------------------------

TEST(VerificationEngine, AcceptsWhenTheWindowMinimumIsBelowTheBound) {
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F), cand(2, 7.0, 0.4F)};

    const VerificationResult r = engine.verify(window, QueueBound::at_least(6.0));

    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.node, 1U);
    EXPECT_EQ(r.key, 5.0);
    EXPECT_TRUE(r.prediction_was_correct);
    EXPECT_EQ(r.rank_displacement, 0U);
}

TEST(VerificationEngine, RefusesWhenSomethingOutsideTheWindowMayBeSmaller) {
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F), cand(2, 7.0, 0.4F)};

    // Something unexamined could hold a key as low as 3.0.
    const VerificationResult r = engine.verify(window, QueueBound::at_least(3.0));

    EXPECT_FALSE(r.accepted());
    EXPECT_EQ(r.outcome, VerificationOutcome::kWindowTooNarrow);
    EXPECT_EQ(engine.metrics().window_too_narrow, 1U);
}

TEST(VerificationEngine, AcceptsOnExactEqualityWithTheBound) {
    // The bound is a LOWER bound, so a key equal to it is still minimal: no
    // unexamined element can be strictly smaller.
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F)};
    EXPECT_TRUE(engine.verify(window, QueueBound::at_least(5.0)).accepted());
}

TEST(VerificationEngine, AcceptsUnconditionallyWhenTheWindowHoldsTheWholeQueue) {
    // Near the end of a search the queue is often smaller than k, and there is
    // then nothing unexamined for a bound to describe.
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 900.0, 0.9F), cand(2, 5.0, 0.1F)};

    const VerificationResult r = engine.verify(window, QueueBound::whole_queue());
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.node, 2U);  // smallest key, not the model's favourite
}

TEST(VerificationEngine, ReportsAnEmptyWindow) {
    VerificationEngine engine;
    const VerificationResult r = engine.verify(std::span<const Candidate>{},
                                               QueueBound::at_least(0.0));
    EXPECT_EQ(r.outcome, VerificationOutcome::kEmptyWindow);
    EXPECT_FALSE(r.accepted());
}

// -----------------------------------------------------------------------------
// The bound is the caller's obligation, and bad bounds are refused
// -----------------------------------------------------------------------------

TEST(VerificationEngine, RefusesANonFiniteBound) {
    // +infinity would accept every candidate unconditionally, which is exactly
    // the failure this component exists to prevent. NaN would make every
    // comparison false and reject forever, presenting as an unexplained
    // fallback to the classical queue.
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F)};

    for (double bad : {std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        const VerificationResult r = engine.verify(window, QueueBound::at_least(bad));
        EXPECT_FALSE(r.accepted()) << "bound " << bad;
    }
    EXPECT_EQ(engine.metrics().rejected_invalid_bound, 3U);
}

TEST(VerificationEngine, TheWholeQueueFlagIsNotDefeatedByItsSentinelBound) {
    // QueueBound::whole_queue() carries -infinity, which is non-finite. The
    // flag must short-circuit before the finiteness guard, or every end-of-
    // search extraction would be refused.
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F)};
    EXPECT_TRUE(engine.verify(window, QueueBound::whole_queue()).accepted());
    EXPECT_EQ(engine.metrics().rejected_invalid_bound, 0U);
}

// -----------------------------------------------------------------------------
// Prediction quality affects speed, never correctness
// -----------------------------------------------------------------------------

TEST(VerificationEngine, AcceptsTheSmallestKeyEvenWhenTheModelPreferredAnother) {
    VerificationEngine engine;
    // The model's favourite (node 1) is not the minimum; node 3 is.
    const std::vector<Candidate> window = {cand(1, 50.0, 0.99F), cand(2, 20.0, 0.50F),
                                           cand(3, 10.0, 0.10F)};

    const VerificationResult r = engine.verify(window, QueueBound::at_least(100.0));

    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.node, 3U);
    EXPECT_FALSE(r.prediction_was_correct);
    EXPECT_EQ(r.rank_displacement, 2U);
    EXPECT_NEAR(r.key_displacement, 40.0, 1e-12);  // 50 - 10
    EXPECT_EQ(engine.metrics().corrections, 1U);
}

// A predictor with no signal at all still cannot cause a wrong extraction.
TEST(VerificationEngine, AWorthlessPredictorStillYieldsCorrectExtractions) {
    Rng                rng(20260922U);
    VerificationEngine engine;

    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<Candidate> window;
        for (NodeId u = 0; u < 8; ++u) {
            // Scores are pure noise, uncorrelated with the keys.
            window.push_back(cand(u, rng.next_double(0.0, 100.0),
                                  static_cast<float>(rng.next_double(0.0, 1.0))));
        }
        std::sort(window.begin(), window.end(), CandidateBetter{});

        const VerificationResult r = engine.verify(window, QueueBound::whole_queue());
        ASSERT_TRUE(r.accepted()) << "trial " << trial;

        const auto true_min = std::min_element(
            window.begin(), window.end(),
            [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
        EXPECT_EQ(r.node, true_min->node) << "trial " << trial;
    }

    // The predictor was noise, so most extractions needed a correction. That
    // shows up as cost, which is the point.
    EXPECT_GT(engine.metrics().corrections, engine.metrics().predictions_correct);
}

TEST(VerificationEngine, BreaksKeyTiesDeterministically) {
    VerificationEngine engine;

    const std::vector<Candidate> forward  = {cand(7, 5.0, 0.9F), cand(3, 5.0, 0.8F)};
    const std::vector<Candidate> backward = {cand(3, 5.0, 0.9F), cand(7, 5.0, 0.8F)};

    EXPECT_EQ(engine.verify(forward, QueueBound::whole_queue()).node, 3U);
    EXPECT_EQ(engine.verify(backward, QueueBound::whole_queue()).node, 3U);
}

// -----------------------------------------------------------------------------
// Soundness against brute force
//
// The argument in the header is that a window minimum at or below a valid lower
// bound on everything unexamined IS the global minimum. This checks that claim
// against the actual global minimum of a randomly generated queue.
// -----------------------------------------------------------------------------

TEST(VerificationSoundness, AcceptedExtractionIsAlwaysTheTrueMinimum) {
    Rng                rng(1002U);  // the differential regression seed
    VerificationEngine engine;

    std::uint64_t accepted_count = 0;
    std::uint64_t refused_count  = 0;

    for (int trial = 0; trial < 5000; ++trial) {
        const std::size_t queue_size = 1 + static_cast<std::size_t>(rng.next_below(40));
        const std::size_t window_size =
            1 + static_cast<std::size_t>(rng.next_below(queue_size));

        // A whole queue of keys. Drawn from a small integer range so exact
        // ties -- the case a sloppy comparison gets wrong -- are common.
        std::vector<Candidate> queue;
        for (std::size_t i = 0; i < queue_size; ++i) {
            queue.push_back(cand(static_cast<NodeId>(i),
                                 static_cast<double>(rng.next_below(12)),
                                 static_cast<float>(rng.next_double(0.0, 1.0))));
        }

        // Split arbitrarily: the window is whatever the (possibly useless)
        // model happened to select, not the smallest keys.
        std::vector<Candidate> shuffled = queue;
        rng.shuffle(shuffled);

        const std::vector<Candidate> window(shuffled.begin(),
                                            shuffled.begin() + static_cast<std::ptrdiff_t>(window_size));
        const std::vector<Candidate> rest(shuffled.begin() + static_cast<std::ptrdiff_t>(window_size),
                                          shuffled.end());

        QueueBound bound = QueueBound::whole_queue();
        if (!rest.empty()) {
            // The tightest valid lower bound on the unexamined part.
            double lowest = rest.front().key;
            for (const Candidate& c : rest) {
                lowest = std::min(lowest, c.key);
            }
            bound = QueueBound::at_least(lowest);
        }

        std::vector<Candidate> ordered = window;
        std::sort(ordered.begin(), ordered.end(), CandidateBetter{});

        const VerificationResult r = engine.verify(ordered, bound);

        // Ground truth: the smallest key in the WHOLE queue, ties on node id.
        const Candidate* global_min = &queue.front();
        for (const Candidate& c : queue) {
            if (c.key < global_min->key ||
                (c.key == global_min->key && c.node < global_min->node)) {
                global_min = &c;
            }
        }

        if (r.accepted()) {
            ++accepted_count;
            // The property the design rests on.
            EXPECT_EQ(r.key, global_min->key)
                << "trial " << trial << ": accepted a key that was not the queue minimum";
        } else {
            ++refused_count;
        }
    }

    // Both branches must actually be exercised, or the test would pass by
    // never accepting anything.
    EXPECT_GT(accepted_count, 0U);
    EXPECT_GT(refused_count, 0U);
}

// The complement: a verifier that accepted everything would pass the test above
// only if that test never generated a case where the minimum lies outside the
// window. This confirms such cases occur and are refused.
TEST(VerificationSoundness, RefusesWhenTheMinimumLiesOutsideTheWindow) {
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 10.0, 0.9F), cand(2, 12.0, 0.5F)};

    // The unexamined part holds a 4.0.
    EXPECT_FALSE(engine.verify(window, QueueBound::at_least(4.0)).accepted());
}

// -----------------------------------------------------------------------------
// Approximate mode
// -----------------------------------------------------------------------------

TEST(VerificationEngine, ApproximateModeAcceptsWithinTheSlack) {
    VerificationEngine engine{CorrectnessPolicy::approximate(0.5)};
    const std::vector<Candidate> window = {cand(1, 14.0, 0.9F)};

    // The true minimum could be as low as 10; 14 <= 1.5 * 10, so accept.
    EXPECT_TRUE(engine.verify(window, QueueBound::at_least(10.0)).accepted());
}

TEST(VerificationEngine, ApproximateModeRefusesBeyondTheSlack) {
    VerificationEngine engine{CorrectnessPolicy::approximate(0.5)};
    const std::vector<Candidate> window = {cand(1, 16.0, 0.9F)};

    // 16 > 1.5 * 10.
    EXPECT_FALSE(engine.verify(window, QueueBound::at_least(10.0)).accepted());
}

// The per-extraction bound IS formally guaranteed, so it must hold on every
// acceptance, not merely usually.
TEST(VerificationEngine, ApproximateModeNeverExceedsThePerExtractionBound) {
    Rng rng(4242U);

    for (double epsilon : {0.01, 0.1, 0.5, 2.0}) {
        VerificationEngine engine{CorrectnessPolicy::approximate(epsilon)};

        for (int trial = 0; trial < 2000; ++trial) {
            const double bound = rng.next_double(0.0, 100.0);

            std::vector<Candidate> window;
            for (NodeId u = 0; u < 5; ++u) {
                window.push_back(cand(u, rng.next_double(0.0, 400.0),
                                      static_cast<float>(rng.next_double(0.0, 1.0))));
            }
            std::sort(window.begin(), window.end(), CandidateBetter{});

            const VerificationResult r = engine.verify(window, QueueBound::at_least(bound));
            if (!r.accepted()) {
                continue;
            }

            // The smallest key the queue could possibly hold.
            double smallest_in_window = window.front().key;
            for (const Candidate& c : window) {
                smallest_in_window = std::min(smallest_in_window, c.key);
            }
            const double best_possible = std::min(smallest_in_window, bound);

            EXPECT_LE(r.key, (1.0 + epsilon) * best_possible + 1e-9)
                << "epsilon " << epsilon << ", trial " << trial;
        }
    }
}

TEST(VerificationEngine, ALargerEpsilonAcceptsAtLeastAsOften) {
    // Monotonicity in the budget. A larger error allowance that refused an
    // extraction a smaller one accepted would make the epsilon sweep in the
    // experiment suite uninterpretable.
    Rng rng(77U);

    VerificationEngine tight{CorrectnessPolicy::approximate(0.05)};
    VerificationEngine loose{CorrectnessPolicy::approximate(0.50)};

    for (int trial = 0; trial < 3000; ++trial) {
        const double bound = rng.next_double(1.0, 50.0);
        const std::vector<Candidate> window = {
            cand(1, rng.next_double(0.0, 100.0), 0.9F)};

        const bool tight_ok = tight.verify(window, QueueBound::at_least(bound)).accepted();
        const bool loose_ok = loose.verify(window, QueueBound::at_least(bound)).accepted();

        if (tight_ok) {
            EXPECT_TRUE(loose_ok) << "trial " << trial;
        }
    }
}

TEST(VerificationEngine, ExactModeAppliesNoSlackAtAll) {
    // Exact mode must reject what approximate mode would accept, otherwise the
    // "exact" label means nothing.
    VerificationEngine exact;
    VerificationEngine approximate{CorrectnessPolicy::approximate(0.2)};

    const std::vector<Candidate> window = {cand(1, 11.0, 0.9F)};
    EXPECT_FALSE(exact.verify(window, QueueBound::at_least(10.0)).accepted());
    EXPECT_TRUE(approximate.verify(window, QueueBound::at_least(10.0)).accepted());
}

// -----------------------------------------------------------------------------
// Budget (ESD.md section 17)
// -----------------------------------------------------------------------------

TEST(VerificationEngine, StopsWhenTheVerificationBudgetIsSpent) {
    CorrectnessPolicy policy;
    policy.verification_budget = 10;
    VerificationEngine engine(policy);

    const std::vector<Candidate> window = {cand(1, 5.0, 0.9F)};

    EXPECT_TRUE(engine.verify(window, QueueBound::at_least(9.0), /*spent=*/9).accepted());

    const VerificationResult r = engine.verify(window, QueueBound::at_least(9.0), 10);
    EXPECT_EQ(r.outcome, VerificationOutcome::kBudgetExhausted);
    EXPECT_EQ(engine.metrics().budget_exhausted, 1U);
}

// -----------------------------------------------------------------------------
// Metrics (PRD.md section 13)
// -----------------------------------------------------------------------------

TEST(VerificationMetrics, CountComparisonsPerVerification) {
    VerificationEngine engine;
    const std::vector<Candidate> window = {cand(1, 1.0, 0.9F), cand(2, 2.0, 0.5F),
                                           cand(3, 3.0, 0.1F)};

    const VerificationResult r = engine.verify(window, QueueBound::at_least(10.0));

    // Two comparisons to scan three candidates, plus one against the bound.
    EXPECT_EQ(r.comparisons, 3U);
    EXPECT_EQ(engine.metrics().comparisons, 3U);
    EXPECT_NEAR(engine.metrics().mean_comparisons(), 3.0, 1e-12);
}

TEST(VerificationMetrics, TrackPredictionAccuracyAndCorrectionRate) {
    VerificationEngine engine;

    // Model right.
    (void)engine.verify(std::vector<Candidate>{cand(1, 1.0, 0.9F), cand(2, 2.0, 0.5F)},
                        QueueBound::whole_queue());
    // Model wrong.
    (void)engine.verify(std::vector<Candidate>{cand(1, 9.0, 0.9F), cand(2, 2.0, 0.5F)},
                        QueueBound::whole_queue());

    const VerificationMetrics& m = engine.metrics();
    EXPECT_EQ(m.accepted, 2U);
    EXPECT_EQ(m.predictions_correct, 1U);
    EXPECT_EQ(m.corrections, 1U);
    EXPECT_NEAR(m.prediction_accuracy(), 0.5, 1e-12);
    EXPECT_NEAR(m.correction_rate(), 0.5, 1e-12);
    EXPECT_EQ(m.max_rank_displacement, 1U);
    EXPECT_NEAR(m.mean_rank_displacement(), 0.5, 1e-12);
    EXPECT_NEAR(m.key_displacement_sum, 7.0, 1e-12);  // 0 + (9 - 2)
}

// An unverifiable extraction has no ground truth to be right or wrong about, so
// counting it either way would distort the accuracy rate.
TEST(VerificationMetrics, RefusedVerificationsDoNotEnterTheAccuracyRate) {
    VerificationEngine engine;

    (void)engine.verify(std::vector<Candidate>{cand(1, 1.0, 0.9F)}, QueueBound::whole_queue());
    (void)engine.verify(std::vector<Candidate>{cand(1, 50.0, 0.9F)},
                        QueueBound::at_least(1.0));

    EXPECT_EQ(engine.metrics().verifications, 2U);
    EXPECT_EQ(engine.metrics().accepted, 1U);
    EXPECT_EQ(engine.metrics().window_too_narrow, 1U);
    EXPECT_NEAR(engine.metrics().prediction_accuracy(), 1.0, 1e-12);
}

TEST(VerificationMetrics, ResetClearsEverything) {
    VerificationEngine engine;
    (void)engine.verify(std::vector<Candidate>{cand(1, 1.0, 0.9F)}, QueueBound::whole_queue());
    ASSERT_GT(engine.metrics().verifications, 0U);

    engine.reset_metrics();
    EXPECT_EQ(engine.metrics().verifications, 0U);
    EXPECT_NEAR(engine.metrics().prediction_accuracy(), 0.0, 1e-12);
    EXPECT_NEAR(engine.metrics().mean_comparisons(), 0.0, 1e-12);
}

// -----------------------------------------------------------------------------
// Integration with the candidate manager
// -----------------------------------------------------------------------------

TEST(VerificationEngine, VerifiesACandidateManagerWindowDirectly) {
    CandidateWindowConfig config;
    config.k_min = 4;
    config.k_max = 4;
    CandidateManager manager(config);
    manager.begin_extraction(16);
    (void)manager.offer(cand(1, 50.0, 0.9F));
    (void)manager.offer(cand(2, 10.0, 0.2F));

    VerificationEngine       engine;
    const VerificationResult r = engine.verify(manager, QueueBound::at_least(20.0));

    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.node, 2U);
    EXPECT_EQ(r.node, manager.min_key_candidate().node);
    EXPECT_NE(r.node, manager.best().node);
}

// The expansion loop from ESD.md section 16: verify, widen on failure, retry.
TEST(VerificationEngine, ExpandingTheWindowCanTurnARefusalIntoAnAcceptance) {
    CandidateWindowConfig config;
    config.k_min            = 1;
    config.k_max            = 8;
    config.expansion_factor = 2;
    config.max_expansions   = 3;

    CandidateManager manager(config);
    manager.begin_extraction(1);  // opens at k_min = 1

    // The model's favourite has a large key; the real minimum scores poorly and
    // does not fit in a window of one.
    (void)manager.offer(cand(1, 90.0, 0.99F));
    (void)manager.offer(cand(2, 5.0, 0.01F));
    ASSERT_EQ(manager.size(), 1U);

    VerificationEngine engine;
    EXPECT_FALSE(engine.verify(manager, QueueBound::at_least(50.0)).accepted());

    ASSERT_TRUE(manager.expand());
    (void)manager.offer(cand(2, 5.0, 0.01F));
    ASSERT_EQ(manager.size(), 2U);

    const VerificationResult r = engine.verify(manager, QueueBound::at_least(50.0));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.node, 2U);
    EXPECT_EQ(r.rank_displacement, 1U);
}

}  // namespace
}  // namespace lpq
