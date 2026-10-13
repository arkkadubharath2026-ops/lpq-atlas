// =============================================================================
// The learner's bounds and its recovery from numerical failure (M5-O3).
//
// A learner inside a search must never be able to stall it, corrupt it, or lose count of what it
// did. These tests pin down: every recorded event is accounted for exactly once; a batch that leaves
// the parameters non-finite or out of range is rolled back to the exact previous parameters; after
// max_rollbacks the learner halts and the model stays where it was; and none of that changes what a
// search returns.
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "lpq/adaptation/online_learner.hpp"
#include "lpq/adaptation/prediction_event.hpp"
#include "lpq/models/logistic_predictor.hpp"

namespace lpq {
namespace {

PredictionEvent positive(float feature = 1.0F) {
    PredictionEvent e;
    e.was_correct = true;
    set_feature(e.features, Feature::kOutDegree, feature);
    return e;
}

PredictionEvent wrong(float first = 1.0F, float accepted = 0.25F) {
    PredictionEvent e;
    e.was_correct          = false;
    e.has_accepted_example = true;
    set_feature(e.features, Feature::kOutDegree, first);
    set_feature(e.accepted_features, Feature::kOutDegree, accepted);
    return e;
}

OnlineLearningConfig cfg(UpdateMode mode = UpdateMode::kPeriodic) {
    OnlineLearningConfig c;
    c.mode            = mode;
    c.interval        = 4;
    c.buffer_capacity = 32;
    c.max_batch       = 8;
    return c;
}

TEST(LearnerExamples, AWrongFirstChoiceAppliesTwoExamples) {
    LogisticPredictor model;
    OnlineLearner     learner(cfg());
    learner.record(wrong());
    learner.record(positive());
    EXPECT_EQ(learner.metrics().examples_recorded, 3U);
    (void)learner.flush(model);
    EXPECT_EQ(learner.metrics().events_applied, 2U);
    EXPECT_EQ(learner.metrics().examples_applied, 3U);
    EXPECT_TRUE(learner.accounting_balanced());
}

TEST(LearnerExamples, TheAcceptedNodeIsATrainedPositive) {
    // Two identical wrong events differ in what they teach the model: the negative pushes the first
    // choice's feature direction down and the positive pushes the accepted node's up.
    LogisticPredictor model;
    OnlineLearner     learner(cfg());
    for (int i = 0; i < 6; ++i) {
        learner.record(wrong(/*first=*/1.0F, /*accepted=*/0.0F));
    }
    (void)learner.flush(model);
    FeatureVector high{};
    set_feature(high, Feature::kOutDegree, 1.0F);
    FeatureVector low{};
    EXPECT_LT(model.predict(high).score, model.predict(low).score)
        << "the node that kept being wrong should now score below the one that kept being right";
}

// -----------------------------------------------------------------------------
// Accounting
// -----------------------------------------------------------------------------

TEST(LearnerAccounting, EveryEventIsAccountedForExactlyOnce) {
    for (const UpdateMode mode : {UpdateMode::kDisabled, UpdateMode::kEveryEvent, UpdateMode::kPeriodic,
                                  UpdateMode::kAdaptive}) {
        LogisticPredictor model;
        auto              c = cfg(mode);
        c.buffer_capacity   = 6;  // overflow is certain with 20 events
        OnlineLearner learner(c);
        for (int i = 0; i < 20; ++i) {
            learner.record(i % 3 == 0 ? wrong() : positive());
            EXPECT_TRUE(learner.accounting_balanced()) << update_mode_name(mode) << " after record " << i;
            if (i % 5 == 4) {
                (void)learner.maybe_update(model);
                EXPECT_TRUE(learner.accounting_balanced()) << update_mode_name(mode);
            }
        }
        (void)learner.flush(model);
        EXPECT_TRUE(learner.accounting_balanced()) << update_mode_name(mode);
        EXPECT_EQ(learner.metrics().events_recorded, 20U);
    }
}

TEST(LearnerAccounting, OverflowDropsTheOldestAndCountsThem) {
    LogisticPredictor model;
    auto              c = cfg();
    c.buffer_capacity   = 4;
    c.interval          = 1000;  // nothing is applied during recording
    OnlineLearner learner(c);
    for (int i = 0; i < 10; ++i) learner.record(positive());
    EXPECT_EQ(learner.metrics().events_dropped, 6U);
    EXPECT_EQ(learner.pending(), 4U);
    (void)learner.flush(model);
    EXPECT_EQ(learner.metrics().events_applied, 4U);
    EXPECT_TRUE(learner.accounting_balanced());
}

TEST(LearnerAccounting, ADisabledLearnerRecordsStatisticsAndChangesNothing) {
    LogisticPredictor model;
    const auto        before = parameters_fnv1a64(model);
    OnlineLearner     learner(cfg(UpdateMode::kDisabled));
    for (int i = 0; i < 9; ++i) learner.record(wrong());
    EXPECT_EQ(learner.flush(model), 0U);
    EXPECT_EQ(learner.metrics().events_discarded_disabled, 9U);
    EXPECT_EQ(parameters_fnv1a64(model), before);
    EXPECT_TRUE(learner.accounting_balanced());
}

// -----------------------------------------------------------------------------
// Numerical failure
// -----------------------------------------------------------------------------

TEST(LearnerRecovery, ABatchThatLeavesParametersOutOfRangeIsRolledBackExactly) {
    LogisticPredictor model;
    model.set_weight(Feature::kOutDegree, 0.5F);
    model.set_bias(0.25F);
    const auto before = parameters_fnv1a64(model);

    auto c           = cfg();
    c.max_abs_weight = 1e-9F;  // any real update exceeds it
    c.max_rollbacks  = 5;
    OnlineLearner learner(c);
    for (int i = 0; i < 6; ++i) learner.record(wrong());
    (void)learner.flush(model);

    EXPECT_EQ(parameters_fnv1a64(model), before) << "the pre-batch parameters are restored bit for bit";
    EXPECT_EQ(learner.metrics().updates_performed, 0U) << "a rolled-back batch is not an update";
    EXPECT_EQ(learner.metrics().events_applied, 0U);
    EXPECT_EQ(learner.metrics().examples_applied, 0U);
    EXPECT_GE(learner.metrics().rollbacks, 1U);
    EXPECT_EQ(learner.metrics().events_rolled_back, 6U);
    EXPECT_FALSE(learner.halted());
    EXPECT_TRUE(learner.accounting_balanced());
}

TEST(LearnerRecovery, RepeatedFailureHaltsTheLearnerAndLeavesTheModelAlone) {
    LogisticPredictor model;
    const auto        before = parameters_fnv1a64(model);

    auto c           = cfg();
    c.max_abs_weight = 1e-9F;
    c.max_rollbacks  = 2;
    c.max_batch      = 2;  // several small batches, so several rollbacks happen in one flush
    OnlineLearner learner(c);
    for (int i = 0; i < 12; ++i) learner.record(wrong());
    (void)learner.flush(model);

    EXPECT_TRUE(learner.halted());
    EXPECT_EQ(learner.metrics().rollbacks, 2U);
    EXPECT_EQ(learner.pending(), 0U) << "what was still waiting is discarded, and counted";
    EXPECT_GT(learner.metrics().events_discarded_halted, 0U);
    EXPECT_EQ(parameters_fnv1a64(model), before);
    EXPECT_TRUE(learner.accounting_balanced());

    // Once halted it ignores events and never applies anything, but still counts them.
    learner.record(positive());
    EXPECT_EQ(learner.flush(model), 0U);
    EXPECT_EQ(learner.maybe_update(model), 0U);
    EXPECT_FALSE(learner.update_due());
    EXPECT_TRUE(learner.accounting_balanced());
    EXPECT_EQ(parameters_fnv1a64(model), before);
}

TEST(LearnerRecovery, NonFiniteFeaturesNeverPoisonTheWeights) {
    LogisticPredictor model;
    PredictionEvent   e = positive();
    e.features[0]       = std::numeric_limits<float>::quiet_NaN();
    OnlineLearner learner(cfg());
    for (int i = 0; i < 5; ++i) learner.record(e);
    (void)learner.flush(model);
    for (const float w : model.weights()) EXPECT_TRUE(std::isfinite(w));
    EXPECT_TRUE(std::isfinite(model.bias()));
    EXPECT_TRUE(learner.accounting_balanced());
}

TEST(LearnerRecovery, ResetClearsTheHalt) {
    LogisticPredictor model;
    auto              c = cfg();
    c.max_abs_weight    = 1e-9F;
    c.max_rollbacks     = 1;
    OnlineLearner learner(c);
    learner.record(wrong());
    (void)learner.flush(model);
    ASSERT_TRUE(learner.halted());
    learner.reset();
    EXPECT_FALSE(learner.halted());
    EXPECT_EQ(learner.metrics().events_recorded, 0U);
}

// -----------------------------------------------------------------------------
// Bounds and determinism
// -----------------------------------------------------------------------------

TEST(LearnerBounds, NoUpdateStepEverAppliesMoreThanMaxBatch) {
    LogisticPredictor model;
    auto              c = cfg();
    c.max_batch         = 3;
    c.buffer_capacity   = 64;
    OnlineLearner learner(c);
    for (int i = 0; i < 50; ++i) learner.record(positive());
    (void)learner.flush(model);
    EXPECT_LE(learner.metrics().largest_batch, 3U);
    EXPECT_GE(learner.metrics().updates_performed, 17U);
}

TEST(LearnerBounds, TheNewConfigurationFieldsAreValidated) {
    OnlineLearningConfig c;
    c.max_abs_weight = 0.0F;
    EXPECT_THROW(OnlineLearner{c}, InvalidConfigError);
    c.max_abs_weight = std::numeric_limits<float>::infinity();
    EXPECT_THROW(OnlineLearner{c}, InvalidConfigError);
    c = OnlineLearningConfig{};
    c.max_rollbacks = 0;
    EXPECT_THROW(OnlineLearner{c}, InvalidConfigError);
}

TEST(LearnerDeterminism, TheSameEventsGiveTheSameParametersAndTheSameHash) {
    const auto run = [] {
        LogisticPredictor model;
        OnlineLearner     learner(cfg());
        for (int i = 0; i < 40; ++i) {
            learner.record(i % 4 == 0 ? wrong(1.0F, 0.5F) : positive(0.75F));
            if (i % 7 == 6) (void)learner.maybe_update(model);
        }
        (void)learner.flush(model);
        return parameters_fnv1a64(model);
    };
    EXPECT_EQ(run(), run());
}

TEST(ParametersHash, ChangesWithEveryParameterAndOnlyThen) {
    LogisticPredictor a;
    LogisticPredictor b;
    EXPECT_EQ(parameters_fnv1a64(a), parameters_fnv1a64(b));
    b.set_bias(0.5F);
    EXPECT_NE(parameters_fnv1a64(a), parameters_fnv1a64(b));
    LogisticPredictor c;
    c.set_weight(Feature::kDegreeRelativeToMean, 1e-6F);
    EXPECT_NE(parameters_fnv1a64(a), parameters_fnv1a64(c));
    EXPECT_TRUE(parameters_within(a, 1.0F));
    a.set_weight(Feature::kOutDegree, std::numeric_limits<float>::infinity());
    EXPECT_FALSE(parameters_within(a, 1e30F));
}

}  // namespace
}  // namespace lpq
