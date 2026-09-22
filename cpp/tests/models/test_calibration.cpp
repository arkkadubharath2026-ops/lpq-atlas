// =============================================================================
// Confidence calibration (PRD.md section 12).
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/models/calibration.hpp"

namespace lpq {
namespace {

// -----------------------------------------------------------------------------
// The logistic function and its inverse
// -----------------------------------------------------------------------------

TEST(Logistic, MatchesTheDefinitionOnOrdinaryInputs) {
    EXPECT_NEAR(logistic(0.0F), 0.5F, 1e-6F);
    EXPECT_NEAR(logistic(1.0F), 1.0F / (1.0F + std::exp(-1.0F)), 1e-6F);
    EXPECT_NEAR(logistic(-1.0F), 1.0F - logistic(1.0F), 1e-6F);
}

TEST(Logistic, IsFiniteAndInRangeAtExtremes) {
    // The naive 1/(1+exp(-z)) form overflows exp() here. The branched
    // implementation must not.
    for (float z : {-1e30F, -800.0F, -100.0F, 100.0F, 800.0F, 1e30F}) {
        const float p = logistic(z);
        EXPECT_TRUE(std::isfinite(p)) << "z = " << z;
        EXPECT_GE(p, 0.0F) << "z = " << z;
        EXPECT_LE(p, 1.0F) << "z = " << z;
    }
}

TEST(Logistic, IsMonotoneIncreasing) {
    float previous = -1.0F;
    for (int i = -200; i <= 200; ++i) {
        const float p = logistic(static_cast<float>(i) * 0.1F);
        EXPECT_GE(p, previous);
        previous = p;
    }
}

TEST(Logit, InvertsLogisticOnTheInteriorRange) {
    for (int i = -50; i <= 50; ++i) {
        const float z = static_cast<float>(i) * 0.2F;
        EXPECT_NEAR(logit(logistic(z)), z, 1e-3F) << "z = " << z;
    }
}

TEST(Logit, IsFiniteAtTheAsymptotes) {
    // An infinite logit would propagate into a weight update and destroy the
    // model rather than perturbing it, so the clamp is load-bearing.
    EXPECT_TRUE(std::isfinite(logit(0.0F)));
    EXPECT_TRUE(std::isfinite(logit(1.0F)));
    EXPECT_LT(logit(0.0F), 0.0F);
    EXPECT_GT(logit(1.0F), 0.0F);
}

// -----------------------------------------------------------------------------
// Temperature scaling
// -----------------------------------------------------------------------------

TEST(TemperatureScaler, DefaultsToTheIdentity) {
    const TemperatureScaler s;
    EXPECT_TRUE(s.is_identity());
    EXPECT_NEAR(s.temperature(), 1.0F, 1e-6F);
    EXPECT_NEAR(s.calibrate_logit(1.5F), logistic(1.5F), 1e-6F);
}

TEST(TemperatureScaler, HighTemperatureSoftensConfidenceTowardsAHalf) {
    const TemperatureScaler cold(1.0F);
    const TemperatureScaler warm(5.0F);
    const float             z = 3.0F;

    const float sharp = cold.calibrate_logit(z);
    const float soft  = warm.calibrate_logit(z);

    EXPECT_GT(sharp, soft);
    EXPECT_GT(soft, 0.5F);
}

TEST(TemperatureScaler, LowTemperatureSharpensConfidence) {
    const TemperatureScaler base(1.0F);
    const TemperatureScaler sharp(0.25F);
    EXPECT_GT(sharp.calibrate_logit(2.0F), base.calibrate_logit(2.0F));
}

// The property the whole design rests on: calibration adjusts confidence
// without ever changing which candidate ranks higher. If this failed,
// recalibrating a model could silently alter the search path.
TEST(TemperatureScaler, NeverChangesTheOrderOfTwoPredictions) {
    Rng rng(20260922U);
    for (float t : {0.1F, 0.5F, 1.0F, 2.0F, 10.0F}) {
        const TemperatureScaler s(t);
        for (int trial = 0; trial < 2000; ++trial) {
            const auto a = static_cast<float>(rng.next_double(-20.0, 20.0));
            const auto b = static_cast<float>(rng.next_double(-20.0, 20.0));

            const float ca = s.calibrate_logit(a);
            const float cb = s.calibrate_logit(b);

            if (a < b) {
                EXPECT_LE(ca, cb) << "temperature " << t << ", logits " << a << " and " << b;
            } else if (a > b) {
                EXPECT_GE(ca, cb) << "temperature " << t << ", logits " << a << " and " << b;
            }
        }
    }
}

TEST(TemperatureScaler, RejectsATemperatureThatWouldInvertEveryConfidence) {
    // A negative temperature would turn the model's most certain positives into
    // its most certain negatives while leaving every number in a plausible
    // range -- invisible downstream, so it must be refused here.
    //
    // Braces, not parentheses. `TemperatureScaler(x)` where x is an identifier
    // parses as a declaration of a variable called x rather than as a
    // construction, so the assertion would test nothing and pass
    // unconditionally -- which is exactly how the NaN and infinity cases here
    // first behaved. GCC accepted that silently; Clang rejected it.
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    const float infinite     = std::numeric_limits<float>::infinity();

    EXPECT_THROW(TemperatureScaler{-1.0F}, InvalidConfigError);
    EXPECT_THROW(TemperatureScaler{0.0F}, InvalidConfigError);
    EXPECT_THROW(TemperatureScaler{not_a_number}, InvalidConfigError);
    EXPECT_THROW(TemperatureScaler{infinite}, InvalidConfigError);
}

TEST(TemperatureScaler, CalibratesAProbabilityConsistentlyWithALogit) {
    const TemperatureScaler s(2.0F);
    const float             z = 1.25F;
    EXPECT_NEAR(s.calibrate_probability(logistic(z)), s.calibrate_logit(z), 1e-4F);
}

// -----------------------------------------------------------------------------
// Uncertainty
// -----------------------------------------------------------------------------

TEST(Uncertainty, IsMaximalAtAHalfAndZeroAtTheExtremes) {
    EXPECT_NEAR(uncertainty_from_confidence(0.5F), 1.0F, 1e-6F);
    EXPECT_NEAR(uncertainty_from_confidence(0.0F), 0.0F, 1e-6F);
    EXPECT_NEAR(uncertainty_from_confidence(1.0F), 0.0F, 1e-6F);
}

TEST(Uncertainty, IsSymmetricAboutAHalf) {
    // Larger must mean less certain regardless of which class is predicted, so
    // a confidence of 0.1 and one of 0.9 are equally certain.
    for (int i = 0; i <= 50; ++i) {
        const float p = static_cast<float>(i) * 0.01F;
        EXPECT_NEAR(uncertainty_from_confidence(p), uncertainty_from_confidence(1.0F - p),
                    1e-6F);
    }
}

// -----------------------------------------------------------------------------
// Reliability accumulation
// -----------------------------------------------------------------------------

TEST(ReliabilityAccumulator, StartsEmpty) {
    const ReliabilityAccumulator<10> acc;
    EXPECT_EQ(acc.total(), 0U);
    // An empty accumulator reports zero error because it has no evidence of
    // miscalibration -- not because it has evidence of good calibration. The
    // caller distinguishes the two with total().
    EXPECT_EQ(acc.expected_calibration_error(), 0.0);
}

TEST(ReliabilityAccumulator, BinsByConfidence) {
    ReliabilityAccumulator<10> acc;
    acc.record(0.05F, true);
    acc.record(0.95F, false);
    EXPECT_EQ(acc.count(0), 1U);
    EXPECT_EQ(acc.count(9), 1U);
    EXPECT_EQ(acc.total(), 2U);
}

TEST(ReliabilityAccumulator, PlacesBoundaryValuesInsideTheArray) {
    // A confidence of exactly 1.0 would index one past the last bin under a
    // plain multiply-and-truncate.
    ReliabilityAccumulator<10> acc;
    acc.record(1.0F, true);
    acc.record(0.0F, false);
    acc.record(std::numeric_limits<float>::quiet_NaN(), false);
    EXPECT_EQ(acc.total(), 3U);
    EXPECT_EQ(acc.count(9), 1U);
    EXPECT_EQ(acc.count(0), 2U);  // zero and NaN both fold into the first bin
}

TEST(ReliabilityAccumulator, ReportsNearZeroErrorForAPerfectlyCalibratedStream) {
    // Construct a stream that is calibrated by construction: in the bin around
    // confidence c, exactly c of the outcomes are positive.
    ReliabilityAccumulator<10> acc;
    for (int bin = 0; bin < 10; ++bin) {
        const float c = (static_cast<float>(bin) + 0.5F) / 10.0F;
        const int   positives = static_cast<int>(std::lround(static_cast<double>(c) * 1000.0));
        for (int i = 0; i < 1000; ++i) {
            acc.record(c, i < positives);
        }
    }
    EXPECT_EQ(acc.total(), 10000U);
    EXPECT_LT(acc.expected_calibration_error(), 0.01);
}

TEST(ReliabilityAccumulator, DetectsSystematicOverconfidence) {
    // Every prediction claims 0.95 and only half are correct. The expected
    // calibration error must be close to the 0.45 gap; a metric that did not
    // detect this would be useless for the purpose PRD.md section 12 gives it.
    ReliabilityAccumulator<10> acc;
    for (int i = 0; i < 1000; ++i) {
        acc.record(0.95F, i % 2 == 0);
    }
    EXPECT_NEAR(acc.expected_calibration_error(), 0.45, 0.02);
}

TEST(ReliabilityAccumulator, UsesTheBinMeanRatherThanItsMidpoint) {
    // All the mass sits at the very bottom of the 0.9-1.0 bin, and the outcomes
    // match 0.90 exactly. Scoring against the bin midpoint of 0.95 would
    // manufacture a 0.05 error that belongs to the binning, not the model.
    ReliabilityAccumulator<10> acc;
    for (int i = 0; i < 1000; ++i) {
        acc.record(0.90F, i < 900);
    }
    EXPECT_NEAR(acc.mean_confidence(9), 0.90, 1e-4);
    EXPECT_NEAR(acc.observed_frequency(9), 0.90, 1e-4);
    EXPECT_LT(acc.expected_calibration_error(), 1e-3);
}

TEST(ReliabilityAccumulator, ResetClearsEverything) {
    ReliabilityAccumulator<10> acc;
    acc.record(0.7F, true);
    acc.reset();
    EXPECT_EQ(acc.total(), 0U);
    EXPECT_EQ(acc.count(7), 0U);
}

}  // namespace
}  // namespace lpq
