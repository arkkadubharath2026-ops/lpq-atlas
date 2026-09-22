// =============================================================================
// Adaptive controller (ESD.md section 18, PRD.md section 14).
//
// The requirement that gets the most attention here is PRD.md section 14's:
// "The controller must have hysteresis or equivalent stabilization so it does
// not oscillate rapidly between modes." Oscillation is tested for directly,
// against an adversarial signal built to provoke it.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lpq/adaptation/adaptive_controller.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"

namespace lpq {
namespace {

CorrectionOutcome outcome(ResolutionPath path, std::uint32_t rank_displacement = 0) {
    CorrectionOutcome o;
    o.path                              = path;
    o.verification.rank_displacement    = rank_displacement;
    o.verification.prediction_was_correct = (rank_displacement == 0);
    return o;
}

ControllerConfig config(double lower, double upper, std::uint64_t warmup = 4,
                        std::uint64_t dwell = 4) {
    ControllerConfig c;
    c.lower_threshold     = lower;
    c.upper_threshold     = upper;
    c.warmup_extractions  = warmup;
    c.dwell_extractions   = dwell;
    c.alpha               = 0.5;  // react fast, so tests stay short
    return c;
}

/// Feeds `n` extractions that each skipped `skipped` entries for `cost`
/// comparisons.
void feed(AdaptiveController& c, int n, std::uint64_t skipped, std::uint32_t cost,
          ResolutionPath path = ResolutionPath::kFirstAttempt) {
    for (int i = 0; i < n; ++i) {
        c.observe(skipped, cost, outcome(path));
    }
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

TEST(ControllerConfig, RequiresAGenuineHysteresisBand) {
    // Equal or inverted thresholds remove the band entirely, and the controller
    // would flip on every observation that straddles the single boundary.
    ControllerConfig c;

    c                 = ControllerConfig{};
    c.lower_threshold = 1.0;
    c.upper_threshold = 1.0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c                 = ControllerConfig{};
    c.lower_threshold = 5.0;
    c.upper_threshold = 1.0;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

TEST(ControllerConfig, RequiresTheModesToBeNamedHonestly) {
    // A "conservative" window narrower than the aggressive one would make every
    // experiment that reads the mode names draw the wrong conclusion.
    ControllerConfig c;
    c.aggressive_window   = 16;
    c.conservative_window = 2;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c.conservative_window = 16;
    EXPECT_NO_THROW(c.validate());
}

TEST(ControllerConfig, RejectsAZeroWindow) {
    ControllerConfig c;
    c.aggressive_window = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Gain is measured in comparisons, not in accuracy
// -----------------------------------------------------------------------------

// The distinction the whole controller turns on: a model can be perfectly
// accurate and still lose, because accuracy says nothing about how much work an
// acceptance saved or how much the checking cost.
TEST(AdaptiveController, APerfectlyAccuratePredictorThatSavesNothingIsNotRewarded) {
    AdaptiveController c{config(/*lower=*/0.0, /*upper=*/2.0)};

    // Every prediction correct, but each acceptance skipped 1 entry and cost 5
    // comparisons to verify: a net loss of 4 per extraction.
    feed(c, 200, /*skipped=*/1, /*cost=*/5);

    EXPECT_LT(c.recent_gain(), 0.0);
    EXPECT_NEAR(c.prediction_success_rate(), 1.0, 1e-9);
    EXPECT_EQ(c.mode(), QueueMode::kClassical)
        << "the controller stayed in a learned mode while losing work on every extraction";
}

TEST(AdaptiveController, RewardsRealSavingsEvenWithImperfectPredictions) {
    AdaptiveController c{config(0.0, 2.0)};

    // Half the extractions correct, but an acceptance skips 100 entries for 4
    // comparisons: a large net saving.
    for (int i = 0; i < 300; ++i) {
        c.observe(100, 4,
                  outcome(i % 2 == 0 ? ResolutionPath::kFirstAttempt
                                     : ResolutionPath::kFirstAttemptCorrected,
                          i % 2));
    }

    EXPECT_GT(c.recent_gain(), 2.0);
    EXPECT_EQ(c.mode(), QueueMode::kAggressiveLearned);
}

// -----------------------------------------------------------------------------
// The control law (ESD.md section 18)
// -----------------------------------------------------------------------------

TEST(AdaptiveController, StartsInTheConfiguredMode) {
    ControllerConfig cfg = config(0.0, 2.0);
    cfg.initial_mode     = QueueMode::kConservativeLearned;
    const AdaptiveController c(cfg);
    EXPECT_EQ(c.mode(), QueueMode::kConservativeLearned);
}

// The first extractions run against a cold model and a nearly empty queue and
// are unrepresentative of anything; without a warmup they would decide the mode
// for the whole query.
TEST(AdaptiveController, DoesNotChangeModeBeforeItIsWarm) {
    AdaptiveController c{config(0.0, 2.0, /*warmup=*/1000, /*dwell=*/0)};
    feed(c, 500, /*skipped=*/1000, /*cost=*/1);
    EXPECT_EQ(c.mode(), QueueMode::kHybrid);
    EXPECT_EQ(c.metrics().mode_changes, 0U);
}

TEST(AdaptiveController, EscalatesOneLevelAtATime) {
    AdaptiveController c{config(0.0, 2.0, /*warmup=*/4, /*dwell=*/1)};

    // Start hybrid; a strong gain should walk it up through conservative to
    // aggressive rather than jumping straight there.
    feed(c, 10, 100, 1);
    EXPECT_NE(c.mode(), QueueMode::kClassical);
    feed(c, 200, 100, 1);
    EXPECT_EQ(c.mode(), QueueMode::kAggressiveLearned);
    EXPECT_GE(c.metrics().mode_changes, 2U);
}

// The retreat is deliberately biased: a learned mode that is losing costs time
// on every extraction, while the classical mode is never worse than the queue
// it is built on.
TEST(AdaptiveController, RetreatsAllTheWayToClassicalUnderSustainedLoss) {
    ControllerConfig cfg = config(0.0, 2.0, 4, 1);
    cfg.initial_mode     = QueueMode::kAggressiveLearned;
    AdaptiveController c(cfg);

    feed(c, 400, /*skipped=*/0, /*cost=*/10, ResolutionPath::kFallback);

    EXPECT_EQ(c.mode(), QueueMode::kClassical);
    EXPECT_LT(c.recent_gain(), 0.0);
}

TEST(AdaptiveController, HoldsInsideTheHysteresisBand) {
    AdaptiveController c{config(/*lower=*/-5.0, /*upper=*/5.0, 4, 1)};

    // Gain of exactly +1: inside the band, so nothing should move.
    feed(c, 500, /*skipped=*/3, /*cost=*/2);

    EXPECT_NEAR(c.recent_gain(), 1.0, 1e-6);
    EXPECT_EQ(c.mode(), QueueMode::kHybrid);
    EXPECT_EQ(c.metrics().mode_changes, 0U);
}

// -----------------------------------------------------------------------------
// Stabilisation (PRD.md section 14)
// -----------------------------------------------------------------------------

// The test the requirement exists for. The gain signal is driven hard across
// the whole band, which two thresholds alone would not suppress -- only the
// dwell time does.
TEST(AdaptiveController, DoesNotOscillateUnderASignalThatCrossesTheWholeBand) {
    ControllerConfig cfg = config(/*lower=*/0.0, /*upper=*/2.0, /*warmup=*/4,
                                  /*dwell=*/200);
    cfg.alpha            = 1.0;  // no smoothing at all: the worst case
    AdaptiveController c(cfg);

    for (int i = 0; i < 4000; ++i) {
        // Alternates between a large win and a large loss every observation.
        if (i % 2 == 0) {
            c.observe(1000, 1, outcome(ResolutionPath::kFirstAttempt));
        } else {
            c.observe(0, 50, outcome(ResolutionPath::kFallback));
        }
    }

    // 4000 observations with a dwell of 200 permits at most 20 changes.
    EXPECT_LE(c.metrics().mode_changes, 20U)
        << "the controller changed mode " << c.metrics().mode_changes
        << " times; the dwell time is not holding";
    EXPECT_GT(c.metrics().suppressed_by_dwell, 0U)
        << "the dwell suppression never engaged, so this test proved nothing";
}

TEST(AdaptiveController, ReportsHowOftenDwellSuppressedASwitch) {
    // A high count means the gain is oscillating across the whole band and the
    // band is too narrow for this workload -- something an experimenter needs
    // to see rather than feel as jitter.
    ControllerConfig cfg = config(0.0, 2.0, 4, /*dwell=*/1000);
    cfg.alpha            = 1.0;
    AdaptiveController c(cfg);

    for (int i = 0; i < 500; ++i) {
        c.observe(i % 2 == 0 ? 1000 : 0, i % 2 == 0 ? 1 : 50,
                  outcome(ResolutionPath::kFirstAttempt));
    }
    EXPECT_GT(c.metrics().suppressed_by_dwell, 0U);
}

TEST(AdaptiveController, NeverOscillatesOnRandomNoise) {
    // Pure noise carries no signal, so a controller that changed mode often
    // under it would be reacting to nothing.
    Rng                rng(20260922U);
    ControllerConfig   cfg = config(0.0, 5.0, 100, 100);
    AdaptiveController c(cfg);

    for (int i = 0; i < 20000; ++i) {
        c.observe(rng.next_below(10), static_cast<std::uint32_t>(rng.next_below(10)),
                  outcome(ResolutionPath::kFirstAttempt));
    }

    EXPECT_LE(c.metrics().mode_changes, 20000U / 100U);
}

// -----------------------------------------------------------------------------
// Policy
// -----------------------------------------------------------------------------

TEST(AdaptiveController, EachModeMapsToADistinctPolicy) {
    ControllerConfig cfg     = config(0.0, 2.0);
    cfg.aggressive_window    = 2;
    cfg.conservative_window  = 16;
    AdaptiveController c(cfg);

    c.force_mode(QueueMode::kAggressiveLearned);
    EXPECT_TRUE(c.policy().use_learned_path);
    EXPECT_EQ(c.policy().window_size, 2U);

    c.force_mode(QueueMode::kConservativeLearned);
    EXPECT_TRUE(c.policy().use_learned_path);
    EXPECT_EQ(c.policy().window_size, 16U);
    EXPECT_GT(c.policy().max_expansions, 0U);

    c.force_mode(QueueMode::kHybrid);
    EXPECT_TRUE(c.policy().use_learned_path);
    EXPECT_EQ(c.policy().max_expansions, 0U) << "hybrid must not retry";

    c.force_mode(QueueMode::kClassical);
    EXPECT_FALSE(c.policy().use_learned_path);
}

TEST(AdaptiveController, ForcingAModeResetsTheDwellSoTheChoiceSticks) {
    // The ablation needs each mode measured in isolation; a forced mode that
    // the controller immediately switched away from would measure something
    // else.
    AdaptiveController c{config(0.0, 2.0, /*warmup=*/1, /*dwell=*/1000)};
    c.force_mode(QueueMode::kClassical);

    feed(c, 500, 1000, 1);  // a strong signal to escalate
    EXPECT_EQ(c.mode(), QueueMode::kClassical);
}

// -----------------------------------------------------------------------------
// Statistics required by ESD.md section 18
// -----------------------------------------------------------------------------

TEST(AdaptiveController, TracksEveryStatisticTheSpecificationRequires) {
    AdaptiveController c{config(0.0, 2.0)};

    for (int i = 0; i < 100; ++i) {
        c.observe(10, 2,
                  outcome(i % 4 == 0 ? ResolutionPath::kFallback
                                     : ResolutionPath::kFirstAttempt,
                          static_cast<std::uint32_t>(i % 5)));
        c.observe_inference_time(12.0);
        c.observe_feature_time(30.0);
        c.observe_verification_time(8.0);
        c.observe_queue_size(1024.0);
    }

    EXPECT_GT(c.prediction_success_rate(), 0.0);
    EXPECT_NEAR(c.correction_rate(), 1.0 - c.prediction_success_rate(), 1e-12);
    EXPECT_GT(c.fallback_rate(), 0.0);
    EXPECT_GT(c.mean_rank_error(), 0.0);
    EXPECT_GT(c.p95_rank_error(), 0U);
    EXPECT_NEAR(c.mean_inference_ns(), 12.0, 1e-6);
    EXPECT_NEAR(c.mean_feature_ns(), 30.0, 1e-6);
    EXPECT_NEAR(c.mean_verification_ns(), 8.0, 1e-6);
    EXPECT_NEAR(c.mean_queue_size(), 1024.0, 1e-6);
}

TEST(AdaptiveController, AccountsForEveryExtractionAgainstSomeMode) {
    AdaptiveController c{config(0.0, 2.0, 4, 4)};
    feed(c, 1000, 50, 1);

    std::uint64_t total = 0;
    for (std::uint64_t n : c.metrics().extractions_in_mode) {
        total += n;
    }
    EXPECT_EQ(total, 1000U);
    EXPECT_EQ(c.metrics().observations, 1000U);
}

TEST(AdaptiveController, ResetReturnsToTheInitialMode) {
    ControllerConfig cfg = config(0.0, 2.0, 4, 1);
    cfg.initial_mode     = QueueMode::kHybrid;
    AdaptiveController c(cfg);

    feed(c, 500, 1000, 1);
    ASSERT_NE(c.mode(), QueueMode::kHybrid);

    c.reset();
    EXPECT_EQ(c.mode(), QueueMode::kHybrid);
    EXPECT_EQ(c.metrics().observations, 0U);
    EXPECT_NEAR(c.recent_gain(), 0.0, 1e-12);
}

}  // namespace
}  // namespace lpq
