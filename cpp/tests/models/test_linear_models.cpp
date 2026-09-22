// =============================================================================
// Linear and logistic predictors (ESD.md section 12, PRD.md section 10).
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/logistic_predictor.hpp"

namespace lpq {
namespace {

// -----------------------------------------------------------------------------
// Configuration validation
// -----------------------------------------------------------------------------

TEST(LinearTrainingConfig, RejectsValuesThatWouldCorruptTraining) {
    LinearTrainingConfig c;

    c = LinearTrainingConfig{};
    c.learning_rate = -0.1F;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c = LinearTrainingConfig{};
    c.learning_rate = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c = LinearTrainingConfig{};
    c.l2 = -1.0F;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c = LinearTrainingConfig{};
    c.positive_weight = 0.0F;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c = LinearTrainingConfig{};
    c.max_weight_delta = 0.0F;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

TEST(LinearTrainingConfig, AcceptsTheDefaults) {
    EXPECT_NO_THROW(LinearTrainingConfig{}.validate());
}

// -----------------------------------------------------------------------------
// Inference
// -----------------------------------------------------------------------------

TEST(LinearPredictor, StartsAtZeroAndPredictsZero) {
    const LinearPredictor m;
    FeatureVector         x{};
    x.fill(1.0F);
    EXPECT_EQ(m.predict(x).score, 0.0F);
    EXPECT_NEAR(m.predict(x).confidence, 0.5F, 1e-6F);
    // A model that has learned nothing must report maximal uncertainty rather
    // than a confident 0.5.
    EXPECT_NEAR(m.predict(x).uncertainty, 1.0F, 1e-5F);
}

TEST(LinearPredictor, ComputesTheDotProductPlusBias) {
    LinearPredictor m;
    m.set_weight(Feature::kOutDegree, 2.0F);
    m.set_weight(Feature::kEffectiveKey, -3.0F);
    m.set_bias(0.5F);

    FeatureVector x{};
    set_feature(x, Feature::kOutDegree, 4.0F);
    set_feature(x, Feature::kEffectiveKey, 1.0F);

    EXPECT_NEAR(m.predict(x).score, (2.0F * 4.0F) + (-3.0F * 1.0F) + 0.5F, 1e-5F);
}

// Determinism matters more here than it looks: a score difference in the last
// bits changes the ORDER of two near-tied candidates, which changes the search
// path and the measured runtime.
TEST(LinearPredictor, IsBitwiseDeterministicAcrossRepeatedCalls) {
    LinearPredictor m;
    Rng             rng(777U);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        m.set_weight(static_cast<Feature>(i), static_cast<float>(rng.next_double(-2.0, 2.0)));
    }

    FeatureVector x{};
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        x[i] = static_cast<float>(rng.next_double(-1.0, 1.0));
    }

    const float first = m.predict(x).score;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(m.predict(x).score, first);
    }
}

TEST(LinearPredictor, PredictionDoesNotDependOnEvaluationOrder) {
    // predict() is const, so this cannot fail by construction -- which is the
    // point. The test documents the contract and would catch a future change
    // that introduced mutable state behind it.
    LinearPredictor m;
    m.set_weight(Feature::kQueueSize, 1.5F);

    FeatureVector a{};
    set_feature(a, Feature::kQueueSize, 0.25F);
    FeatureVector b{};
    set_feature(b, Feature::kQueueSize, 0.75F);

    const float a_first = m.predict(a).score;
    (void)m.predict(b);
    EXPECT_EQ(m.predict(a).score, a_first);
}

TEST(LinearPredictor, RejectsAWeightVectorOfTheWrongLength) {
    LinearPredictor m;
    EXPECT_THROW(m.set_weights(std::vector<float>(kFeatureCount - 1, 0.0F)), InvalidConfigError);
    EXPECT_THROW(m.set_weights(std::vector<float>(kFeatureCount + 1, 0.0F)), InvalidConfigError);
    EXPECT_NO_THROW(m.set_weights(std::vector<float>(kFeatureCount, 0.25F)));
    EXPECT_EQ(m.weights()[0], 0.25F);
}

// -----------------------------------------------------------------------------
// Online update: mechanics
// -----------------------------------------------------------------------------

TEST(LinearPredictor, CountsUpdates) {
    LinearPredictor m;
    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree, 1.0F);
    ex.target = 1.0F;

    EXPECT_EQ(m.updates_applied(), 0U);
    m.update(ex);
    m.update(ex);
    EXPECT_EQ(m.updates_applied(), 2U);
}

TEST(LinearPredictor, MovesThePredictionTowardsTheTarget) {
    LinearTrainingConfig config;
    config.learning_rate = 0.1F;
    config.l2            = 0.0F;
    LinearPredictor m(config);

    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree, 1.0F);
    ex.target = 1.0F;

    float previous_error = std::abs(m.predict(ex.features).score - ex.target);
    for (int i = 0; i < 20; ++i) {
        m.update(ex);
        const float error = std::abs(m.predict(ex.features).score - ex.target);
        EXPECT_LT(error, previous_error) << "step " << i;
        previous_error = error;
    }
    EXPECT_LT(previous_error, 0.2F);
}

TEST(LinearPredictor, L2DecayShrinksWeightsWhenThereIsNoGradient) {
    LinearTrainingConfig config;
    config.learning_rate = 0.5F;
    config.l2            = 0.5F;
    LinearPredictor m(config);
    m.set_weight(Feature::kOutDegree, 4.0F);

    // An all-zero example produces no gradient, so any movement is decay.
    TrainingExample ex;
    ex.target = 0.0F;

    const float before = m.weights()[static_cast<std::size_t>(Feature::kOutDegree)];
    m.update(ex);
    const float after = m.weights()[static_cast<std::size_t>(Feature::kOutDegree)];

    EXPECT_LT(after, before);
    EXPECT_GT(after, 0.0F);
}

TEST(LinearPredictor, ClipsASinglePathologicalUpdate) {
    // Without the cap, one example carrying a huge feature value moves a weight
    // far enough to destroy the model in a single step -- and because updates
    // happen inside a running search, there is no epoch boundary at which the
    // damage would be noticed.
    LinearTrainingConfig config;
    config.learning_rate    = 1.0F;
    config.l2               = 0.0F;
    config.max_weight_delta = 0.5F;
    LinearPredictor m(config);

    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree, 1e6F);
    ex.target = 1.0F;
    m.update(ex);

    const float w = m.weights()[static_cast<std::size_t>(Feature::kOutDegree)];
    EXPECT_LE(std::abs(w), 0.5F + 1e-5F);
    EXPECT_TRUE(std::isfinite(w));
}

TEST(LinearPredictor, IgnoresANonFiniteExample) {
    LinearPredictor m;
    m.set_weight(Feature::kOutDegree, 1.0F);
    const float before = m.weights()[static_cast<std::size_t>(Feature::kOutDegree)];

    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree,
                std::numeric_limits<float>::quiet_NaN());
    ex.target = 1.0F;
    m.update(ex);

    EXPECT_EQ(m.weights()[static_cast<std::size_t>(Feature::kOutDegree)], before);
    EXPECT_TRUE(std::isfinite(m.predict(FeatureVector{}).score));
}

TEST(LinearPredictor, SurvivesAStreamOfHostileExamples) {
    // Updates can arrive from a deserialised event log or a Python-generated
    // dataset, neither of which this model controls. No sequence of inputs may
    // leave it producing non-finite scores.
    LinearPredictor m;
    Rng             rng(31337U);

    const std::vector<float> nasty = {std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity(),
                                      -std::numeric_limits<float>::infinity(),
                                      3.4e38F,
                                      -3.4e38F,
                                      0.0F};

    for (int i = 0; i < 5000; ++i) {
        TrainingExample ex;
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            ex.features[f] = nasty[static_cast<std::size_t>(rng.next_below(nasty.size()))];
        }
        ex.target = static_cast<float>(rng.next_below(2));
        ex.weight = nasty[static_cast<std::size_t>(rng.next_below(nasty.size()))];
        m.update(ex);
    }

    FeatureVector probe{};
    probe.fill(0.5F);
    EXPECT_TRUE(std::isfinite(m.predict(probe).score));
    for (float w : m.weights()) {
        EXPECT_TRUE(std::isfinite(w));
    }
}

// -----------------------------------------------------------------------------
// Logistic predictor
// -----------------------------------------------------------------------------

TEST(LogisticPredictor, ReturnsTheLogitAsScoreAndTheSigmoidAsConfidence) {
    LogisticPredictor m;
    m.set_weight(Feature::kOutDegree, 2.0F);

    FeatureVector x{};
    set_feature(x, Feature::kOutDegree, 1.5F);

    const Prediction p = m.predict(x);
    EXPECT_NEAR(p.score, 3.0F, 1e-5F);
    EXPECT_NEAR(p.confidence, logistic(3.0F), 1e-5F);
    EXPECT_NEAR(m.probability(x), logistic(3.0F), 1e-5F);
}

// Ranking on confidence rather than on the logit would make strongly positive
// candidates tie once the sigmoid saturates in float, and the tie-break would
// then decide the search path instead of the model.
TEST(LogisticPredictor, TheLogitSeparatesCandidatesWhoseConfidencesBothSaturate) {
    LogisticPredictor m;
    m.set_weight(Feature::kOutDegree, 1.0F);

    FeatureVector a{};
    set_feature(a, Feature::kOutDegree, 60.0F);
    FeatureVector b{};
    set_feature(b, Feature::kOutDegree, 90.0F);

    const Prediction pa = m.predict(a);
    const Prediction pb = m.predict(b);

    EXPECT_EQ(pa.confidence, 1.0F);
    EXPECT_EQ(pb.confidence, 1.0F);
    EXPECT_LT(pa.score, pb.score);  // still ordered
}

TEST(LogisticPredictor, ConfidenceStaysInRangeForAnyInput) {
    LogisticPredictor m;
    Rng               rng(2024U);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        m.set_weight(static_cast<Feature>(i), static_cast<float>(rng.next_double(-50.0, 50.0)));
    }
    for (int trial = 0; trial < 5000; ++trial) {
        FeatureVector x{};
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            x[i] = static_cast<float>(rng.next_double(-100.0, 100.0));
        }
        const Prediction p = m.predict(x);
        EXPECT_GE(p.confidence, 0.0F);
        EXPECT_LE(p.confidence, 1.0F);
        EXPECT_GE(p.uncertainty, 0.0F);
        EXPECT_LE(p.uncertainty, 1.0F);
        EXPECT_TRUE(std::isfinite(p.score));
    }
}

TEST(LogisticPredictor, PositiveWeightingMovesTheModelFurtherOnPositives) {
    // Without reweighting, a model facing one positive per extraction against
    // many negatives scores well by always answering "no".
    LinearTrainingConfig config;
    config.learning_rate   = 0.1F;
    config.l2              = 0.0F;
    config.positive_weight = 10.0F;

    LogisticPredictor weighted(config);
    config.positive_weight = 1.0F;
    LogisticPredictor plain(config);

    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree, 1.0F);
    ex.target = 1.0F;

    weighted.update(ex);
    plain.update(ex);

    EXPECT_GT(weighted.predict(ex.features).score, plain.predict(ex.features).score);
}

// -----------------------------------------------------------------------------
// Does it actually learn?
//
// A measured claim, not an assumed one. The data is generated from a fixed seed
// so the numbers below are reproducible, and the held-out set is disjoint from
// the training stream.
// -----------------------------------------------------------------------------

struct Dataset {
    std::vector<FeatureVector> x;
    std::vector<float>         y;
};

/// Draws a linearly separable classification problem from a seeded generator.
Dataset make_separable_dataset(Rng& rng, int n, const std::vector<float>& true_weights) {
    Dataset d;
    d.x.reserve(static_cast<std::size_t>(n));
    d.y.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        FeatureVector v{};
        double        z = 0.0;
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            v[f] = static_cast<float>(rng.next_double(-1.0, 1.0));
            z += static_cast<double>(v[f]) * static_cast<double>(true_weights[f]);
        }
        d.x.push_back(v);
        d.y.push_back(z > 0.0 ? 1.0F : 0.0F);
    }
    return d;
}

double accuracy(const LogisticPredictor& m, const Dataset& d) {
    int correct = 0;
    for (std::size_t i = 0; i < d.x.size(); ++i) {
        const bool predicted_positive = m.predict(d.x[i]).score > 0.0F;
        const bool actually_positive  = d.y[i] > 0.5F;
        if (predicted_positive == actually_positive) {
            ++correct;
        }
    }
    return static_cast<double>(correct) / static_cast<double>(d.x.size());
}

TEST(LogisticPredictor, LearnsALinearlySeparableProblemFromAFixedSeed) {
    Rng rng(20260922U);

    std::vector<float> true_weights(kFeatureCount, 0.0F);
    // A sparse target: only a few features carry signal, which is the regime
    // the real feature vector is expected to be in.
    true_weights[static_cast<std::size_t>(Feature::kEffectiveKey)]       = -3.0F;
    true_weights[static_cast<std::size_t>(Feature::kKeyGapFromMinimum)]  = -2.0F;
    true_weights[static_cast<std::size_t>(Feature::kOutDegree)]          = 1.0F;

    const Dataset train = make_separable_dataset(rng, 20000, true_weights);
    const Dataset test  = make_separable_dataset(rng, 5000, true_weights);

    LinearTrainingConfig config;
    config.learning_rate = 0.1F;
    config.l2            = 1e-5F;
    LogisticPredictor m(config);

    const double before = accuracy(m, test);

    for (std::size_t i = 0; i < train.x.size(); ++i) {
        TrainingExample ex;
        ex.features = train.x[i];
        ex.target   = train.y[i];
        m.update(ex);
    }

    const double after = accuracy(m, test);

    // Printed, not just asserted: the numbers a test measures are part of the
    // experimental record, and a bound that is only checked leaves no trace of
    // how much headroom it had.
    std::cout << "[  MEASURED ] held-out accuracy before training: " << before
              << ", after: " << after << std::endl;

    EXPECT_GT(after, 0.90) << "held-out accuracy after training was " << after;
    EXPECT_GT(after, before + 0.20)
        << "training moved held-out accuracy from " << before << " to " << after;
}

TEST(LogisticPredictor, DoesNotLearnFromPureNoise) {
    // The complement of the test above, and the one that makes it meaningful:
    // if the harness reported high accuracy here too, it would be measuring
    // something other than learning.
    Rng rng(555U);

    Dataset d;
    for (int i = 0; i < 20000; ++i) {
        FeatureVector v{};
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            v[f] = static_cast<float>(rng.next_double(-1.0, 1.0));
        }
        d.x.push_back(v);
        d.y.push_back(static_cast<float>(rng.next_below(2)));  // label independent of features
    }

    LinearTrainingConfig config;
    config.learning_rate = 0.1F;
    LogisticPredictor m(config);
    for (std::size_t i = 0; i < d.x.size(); ++i) {
        TrainingExample ex;
        ex.features = d.x[i];
        ex.target   = d.y[i];
        m.update(ex);
    }

    const double acc = accuracy(m, d);
    std::cout << "[  MEASURED ] accuracy on unlearnable data: " << acc << std::endl;
    EXPECT_LT(acc, 0.60) << "accuracy on unlearnable data was " << acc
                         << ", which suggests the evaluation is leaking labels";
}

TEST(LinearPredictor, LearnsARegressionTargetFromAFixedSeed) {
    // The raw-value formulation PRD.md section 8 asks for as an ablation.
    Rng rng(8080U);

    std::vector<float> true_weights(kFeatureCount, 0.0F);
    true_weights[static_cast<std::size_t>(Feature::kTentativeDistance)] = 0.7F;
    true_weights[static_cast<std::size_t>(Feature::kHopDepth)]          = -0.3F;

    LinearTrainingConfig config;
    config.learning_rate = 0.05F;
    config.l2            = 0.0F;
    LinearPredictor m(config);

    double first_half_error = 0.0;
    double last_half_error  = 0.0;
    const int n = 20000;

    for (int i = 0; i < n; ++i) {
        TrainingExample ex;
        double          z = 0.0;
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            ex.features[f] = static_cast<float>(rng.next_double(-1.0, 1.0));
            z += static_cast<double>(ex.features[f]) * static_cast<double>(true_weights[f]);
        }
        ex.target = static_cast<float>(z);

        const double error =
            std::abs(static_cast<double>(m.predict(ex.features).score) - z);
        if (i < n / 2) {
            first_half_error += error;
        } else {
            last_half_error += error;
        }
        m.update(ex);
    }

    first_half_error /= (n / 2);
    last_half_error /= (n / 2);

    std::cout << "[  MEASURED ] mean absolute error, first half: " << first_half_error
              << ", last half: " << last_half_error << std::endl;

    EXPECT_LT(last_half_error, first_half_error * 0.5)
        << "mean absolute error went from " << first_half_error << " to " << last_half_error;
    EXPECT_LT(last_half_error, 0.05);
}

}  // namespace
}  // namespace lpq
