// =============================================================================
// Online learning (ESD.md section 14, PRD.md section 11).
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "lpq/adaptation/online_learner.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/models/logistic_predictor.hpp"

namespace lpq {
namespace {

PredictionEvent event(bool correct, std::uint32_t rank_error = 0, bool fallback = false) {
    PredictionEvent e;
    e.was_correct   = correct;
    e.actual_rank   = rank_error;
    e.rank_error    = rank_error;
    e.fallback_used = fallback;
    set_feature(e.features, Feature::kOutDegree, 1.0F);
    return e;
}

OnlineLearningConfig config(UpdateMode mode, std::uint32_t interval = 4,
                            std::uint32_t capacity = 64) {
    OnlineLearningConfig c;
    c.mode            = mode;
    c.interval        = interval;
    c.buffer_capacity = capacity;
    c.max_batch       = capacity;
    return c;
}

// -----------------------------------------------------------------------------
// Exponentially weighted statistics
// -----------------------------------------------------------------------------

TEST(ExponentiallyWeighted, RejectsAnAlphaOutsideTheOpenUnitInterval) {
    EXPECT_THROW(ExponentiallyWeighted{0.0}, InvalidConfigError);
    EXPECT_THROW(ExponentiallyWeighted{-0.1}, InvalidConfigError);
    EXPECT_THROW(ExponentiallyWeighted{1.5}, InvalidConfigError);
    EXPECT_THROW(ExponentiallyWeighted{std::numeric_limits<double>::quiet_NaN()},
                 InvalidConfigError);
    EXPECT_NO_THROW(ExponentiallyWeighted{1.0});
}

// Seeding from zero would make the statistic report a low error simply because
// it had seen nothing, and a controller reading it would trust the model on the
// strength of no evidence.
TEST(ExponentiallyWeighted, TheFirstObservationBecomesTheValueOutright) {
    ExponentiallyWeighted ew(0.01);
    ew.add(1.0);
    EXPECT_NEAR(ew.value(), 1.0, 1e-12);
    EXPECT_EQ(ew.count(), 1U);
}

TEST(ExponentiallyWeighted, RespondsAtAFixedRateRegardlessOfHistoryLength) {
    // The reason for using this instead of a running mean: after a long run, a
    // plain mean barely moves, so a controller would keep trusting a model that
    // had already stopped working.
    ExponentiallyWeighted ew(0.2);
    for (int i = 0; i < 100000; ++i) {
        ew.add(0.0);
    }
    ASSERT_NEAR(ew.value(), 0.0, 1e-6);

    for (int i = 0; i < 20; ++i) {
        ew.add(1.0);
    }
    EXPECT_GT(ew.value(), 0.9) << "the statistic failed to react to a regime change";
}

TEST(ExponentiallyWeighted, ReportsWarmthSeparatelyFromValue) {
    ExponentiallyWeighted ew(0.5);
    EXPECT_FALSE(ew.warm(10));
    for (int i = 0; i < 10; ++i) {
        ew.add(0.5);
    }
    EXPECT_TRUE(ew.warm(10));
}

// -----------------------------------------------------------------------------
// Bounded histogram
// -----------------------------------------------------------------------------

TEST(BoundedHistogram, ComputesExactQuantiles) {
    BoundedHistogram<64> h;
    for (int i = 0; i < 95; ++i) {
        h.add(1);
    }
    for (int i = 0; i < 5; ++i) {
        h.add(40);
    }
    EXPECT_EQ(h.total(), 100U);
    EXPECT_EQ(h.quantile(0.5), 1U);
    EXPECT_EQ(h.quantile(0.95), 1U);
    EXPECT_EQ(h.quantile(0.99), 40U);
    EXPECT_NEAR(h.mean(), (95.0 + (5.0 * 40.0)) / 100.0, 1e-9);
}

TEST(BoundedHistogram, FoldsOutOfRangeValuesIntoTheTopBucketAndReportsTheRealMaximum) {
    // The fold makes the reported tail conservative -- it can understate how
    // far out an outlier was -- so the true maximum is reported separately
    // rather than lost.
    BoundedHistogram<8> h;
    h.add(3);
    h.add(1000);
    EXPECT_EQ(h.total(), 2U);
    EXPECT_EQ(h.quantile(1.0), 7U);
    EXPECT_EQ(h.maximum(), 1000U);
}

TEST(BoundedHistogram, ReportsZeroWhenEmpty) {
    const BoundedHistogram<16> h;
    EXPECT_EQ(h.total(), 0U);
    EXPECT_EQ(h.quantile(0.95), 0U);
    EXPECT_NEAR(h.mean(), 0.0, 1e-12);
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

TEST(OnlineLearningConfig, RejectsSettingsThatWouldNeverApplyAnUpdate) {
    OnlineLearningConfig c;

    c           = OnlineLearningConfig{};
    c.interval  = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    // max_batch 0 records forever and applies nothing -- kDisabled wearing the
    // wrong label, and it would be reported as a learning configuration.
    c           = OnlineLearningConfig{};
    c.max_batch = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c                 = OnlineLearningConfig{};
    c.buffer_capacity = 0;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c                 = OnlineLearningConfig{};
    c.error_threshold = 1.5;
    EXPECT_THROW(c.validate(), InvalidConfigError);

    c                  = OnlineLearningConfig{};
    c.fallback_penalty = 0.0F;
    EXPECT_THROW(c.validate(), InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Update modes (ESD.md section 14)
// -----------------------------------------------------------------------------

TEST(OnlineLearner, DisabledRecordsStatisticsButNeverUpdates) {
    // The frozen-model control must still pay the observation cost, or the
    // comparison against the learning configurations would be confounded by
    // the bookkeeping rather than by the learning.
    OnlineLearner     learner{config(UpdateMode::kDisabled)};
    LogisticPredictor model;

    for (int i = 0; i < 100; ++i) {
        learner.record(event(i % 2 == 0));
    }

    EXPECT_EQ(learner.metrics().events_recorded, 100U);
    EXPECT_EQ(learner.pending(), 0U);
    EXPECT_FALSE(learner.update_due());
    EXPECT_EQ(learner.maybe_update(model), 0U);
    EXPECT_EQ(learner.flush(model), 0U);
    EXPECT_EQ(model.updates_applied(), 0U);

    // But the statistics are live.
    EXPECT_GT(learner.recent_error_rate(), 0.0);
}

TEST(OnlineLearner, EveryEventAppliesImmediately) {
    OnlineLearner     learner{config(UpdateMode::kEveryEvent)};
    LogisticPredictor model;

    for (int i = 0; i < 10; ++i) {
        learner.record(event(false));
        EXPECT_EQ(learner.maybe_update(model), 1U);
    }
    EXPECT_EQ(model.updates_applied(), 10U);
    EXPECT_EQ(learner.pending(), 0U);
}

TEST(OnlineLearner, PeriodicAppliesOnlyOnTheInterval) {
    OnlineLearner     learner{config(UpdateMode::kPeriodic, /*interval=*/4)};
    LogisticPredictor model;

    for (int i = 0; i < 3; ++i) {
        learner.record(event(false));
        EXPECT_EQ(learner.maybe_update(model), 0U) << "fired early at event " << i;
    }
    learner.record(event(false));
    EXPECT_EQ(learner.maybe_update(model), 4U);
    EXPECT_EQ(model.updates_applied(), 4U);
}

TEST(OnlineLearner, AdaptiveFiresOnlyWhenRecentErrorExceedsTheThreshold) {
    OnlineLearningConfig c = config(UpdateMode::kAdaptive);
    c.error_threshold      = 0.5;
    c.adaptive_warmup      = 10;
    c.error_alpha          = 0.5;  // react fast, so the test is short

    OnlineLearner     learner(c);
    LogisticPredictor model;

    // A run of correct predictions keeps the error low: nothing should fire.
    for (int i = 0; i < 50; ++i) {
        learner.record(event(true));
    }
    EXPECT_FALSE(learner.update_due());
    EXPECT_EQ(learner.maybe_update(model), 0U);

    // Degrade the predictions and it must engage.
    for (int i = 0; i < 20; ++i) {
        learner.record(event(false));
    }
    EXPECT_TRUE(learner.update_due());
    EXPECT_GT(learner.maybe_update(model), 0U);
}

TEST(OnlineLearner, AdaptiveDoesNotFireBeforeItIsWarm) {
    // Without the warmup gate a single unlucky first event would trigger an
    // update on a sample of one.
    OnlineLearningConfig c = config(UpdateMode::kAdaptive);
    c.error_threshold      = 0.1;
    c.adaptive_warmup      = 50;

    OnlineLearner learner(c);
    learner.record(event(false));
    EXPECT_FALSE(learner.update_due());
}

// -----------------------------------------------------------------------------
// Bounded cost (PRD.md section 11)
// -----------------------------------------------------------------------------

// The limit that matters: a long quiet period followed by one update must not
// produce an unbounded pause in the middle of a timed query.
TEST(OnlineLearner, ASingleUpdateNeverExceedsTheBatchLimit) {
    OnlineLearningConfig c = config(UpdateMode::kPeriodic, /*interval=*/1,
                                    /*capacity=*/4096);
    c.max_batch            = 32;

    OnlineLearner     learner(c);
    LogisticPredictor model;

    for (int i = 0; i < 2000; ++i) {
        learner.record(event(false));
    }
    EXPECT_EQ(learner.maybe_update(model), 32U);
    EXPECT_EQ(learner.metrics().largest_batch, 32U);
    EXPECT_EQ(model.updates_applied(), 32U);
}

TEST(OnlineLearner, FlushAppliesEverythingButStillInBoundedBatches) {
    OnlineLearningConfig c = config(UpdateMode::kPeriodic, 1, 4096);
    c.max_batch            = 16;

    OnlineLearner     learner(c);
    LogisticPredictor model;

    for (int i = 0; i < 100; ++i) {
        learner.record(event(false));
    }
    EXPECT_EQ(learner.flush(model), 100U);
    EXPECT_EQ(learner.pending(), 0U);
    EXPECT_EQ(learner.metrics().largest_batch, 16U);
    EXPECT_GE(learner.metrics().updates_performed, 100U / 16U);
}

TEST(OnlineLearner, RecordingNeverAllocatesBeyondTheConfiguredBuffer) {
    OnlineLearner     learner{config(UpdateMode::kPeriodic, 1000000, /*capacity=*/8)};
    const std::size_t reserved = learner.memory_bytes();

    for (int i = 0; i < 10000; ++i) {
        learner.record(event(i % 3 == 0));
        ASSERT_EQ(learner.memory_bytes(), reserved) << "at event " << i;
        ASSERT_LE(learner.pending(), 8U);
    }
}

// Dropping the oldest is deliberate: under distribution shift the oldest events
// describe a regime that no longer exists. The rate is reported rather than
// hidden, because a high one means the configuration is starving the learner.
TEST(OnlineLearner, OverwritesTheOldestEventsAndReportsTheDropRate) {
    OnlineLearner learner{config(UpdateMode::kPeriodic, 1000000, /*capacity=*/10)};

    for (int i = 0; i < 100; ++i) {
        learner.record(event(false));
    }
    EXPECT_EQ(learner.pending(), 10U);
    EXPECT_EQ(learner.metrics().events_dropped, 90U);
    EXPECT_NEAR(learner.metrics().drop_rate(), 0.9, 1e-9);
}

// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------

// SGD is order-dependent, so a learner that reordered events would make two
// runs from the same seed produce different models, different search paths and
// different timings -- while still returning correct answers, which is the
// hardest kind of irreproducibility to notice.
TEST(OnlineLearner, ProducesAnIdenticalModelFromAnIdenticalEventSequence) {
    Rng rng(20260922U);

    std::vector<PredictionEvent> events;
    for (int i = 0; i < 500; ++i) {
        PredictionEvent e = event(rng.next_below(2) == 0,
                                  static_cast<std::uint32_t>(rng.next_below(8)),
                                  rng.next_below(4) == 0);
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            e.features[f] = static_cast<float>(rng.next_double(-1.0, 1.0));
        }
        events.push_back(e);
    }

    auto run = [&events]() {
        OnlineLearner     learner{config(UpdateMode::kPeriodic, 7, 1024)};
        LogisticPredictor model;
        for (const PredictionEvent& e : events) {
            learner.record(e);
            (void)learner.maybe_update(model);
        }
        (void)learner.flush(model);
        return model;
    };

    const LogisticPredictor a = run();
    const LogisticPredictor b = run();

    EXPECT_EQ(a.updates_applied(), b.updates_applied());
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        EXPECT_EQ(a.weights()[i], b.weights()[i]) << "weight " << i;
    }
    EXPECT_EQ(a.bias(), b.bias());
}

// -----------------------------------------------------------------------------
// The labels, and whether learning from them actually helps
// -----------------------------------------------------------------------------

TEST(PredictionEvent, WeighsAFallbackMoreHeavilyThanARepairedMistake) {
    // A wrong first choice that verification repaired inside the window is
    // cheap; one that forced a fallback discarded the prediction and paid the
    // classical cost as well. Treating them alike would train the model to
    // avoid cheap mistakes as hard as expensive ones.
    const TrainingExample repaired = event(false, 2, /*fallback=*/false).to_example(4.0F);
    const TrainingExample fell_back = event(false, 2, /*fallback=*/true).to_example(4.0F);

    EXPECT_EQ(repaired.target, 0.0F);
    EXPECT_EQ(fell_back.target, 0.0F);
    EXPECT_GT(fell_back.weight, repaired.weight);
}

TEST(PredictionEvent, ACorrectPredictionIsAPositiveExample) {
    const TrainingExample ex = event(true).to_example();
    EXPECT_EQ(ex.target, 1.0F);
}

// An end-to-end check that the pipeline learns: events whose label is a
// deterministic function of a feature must drive the model to predict it.
TEST(OnlineLearner, TheEventPipelineActuallyTrainsTheModel) {
    Rng rng(4242U);

    LinearTrainingConfig training;
    training.learning_rate = 0.1F;
    training.l2            = 0.0F;
    LogisticPredictor model(training);

    OnlineLearner learner{config(UpdateMode::kEveryEvent, 1, 1024)};

    int correct_before = 0;
    int correct_after  = 0;

    for (int i = 0; i < 4000; ++i) {
        PredictionEvent e;
        const auto      signal = static_cast<float>(rng.next_double(-1.0, 1.0));
        set_feature(e.features, Feature::kQueueDensity, signal);
        e.was_correct = signal > 0.0F;

        const bool predicted = model.predict(e.features).score > 0.0F;
        if (predicted == e.was_correct) {
            (i < 500 ? correct_before : correct_after) += 1;
        }

        learner.record(e);
        (void)learner.maybe_update(model);
    }

    const double before = static_cast<double>(correct_before) / 500.0;
    const double after  = static_cast<double>(correct_after) / 3500.0;

    std::cout << "[  MEASURED ] event-pipeline accuracy, first 500: " << before
              << ", remaining 3500: " << after << std::endl;

    EXPECT_GT(after, 0.9) << "the pipeline did not learn a deterministic label";
    EXPECT_GT(after, before);
    EXPECT_EQ(learner.metrics().events_applied, 4000U);
}

TEST(OnlineLearner, ResetClearsEverything) {
    OnlineLearner learner{config(UpdateMode::kPeriodic)};
    for (int i = 0; i < 20; ++i) {
        learner.record(event(false, 3));
    }
    ASSERT_GT(learner.metrics().events_recorded, 0U);

    learner.reset();
    EXPECT_EQ(learner.metrics().events_recorded, 0U);
    EXPECT_EQ(learner.pending(), 0U);
    EXPECT_NEAR(learner.recent_error_rate(), 0.0, 1e-12);
    EXPECT_EQ(learner.p95_rank_error(), 0U);
}

}  // namespace
}  // namespace lpq
