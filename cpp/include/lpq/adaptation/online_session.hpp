// =============================================================================
// One online-learning session over one graph (M5-O4).
//
// Owns the live model, the learner and the scorer that reads the model, and applies updates
// BETWEEN queries: during a search the model is constant, so every push in that search sees the
// same scores, and the search is exactly the one a frozen lpq_model would run. The learner only
// records what the verifier established. After the search, end_query() applies the recorded events
// in bounded batches.
//
// What a session promises
// -------------------------------------------------------------------------------------------
//   * Correctness never depends on the model. Scores order the examination of entries; keys decide.
//   * Within a query, scores are identical to the frozen model's (nothing is applied mid-search).
//   * An update is bounded (OnlineLearner::max_batch per step, a fixed-size ring buffer), is applied
//     in arrival order, and is rolled back if it leaves non-finite or absurd parameters. After
//     max_rollbacks the learner halts and the model stays at its last good parameters.
//   * Nothing here says an update improves anything. weights_changed() only says the parameters
//     differ from where the session started.
//
// The session is not copyable or movable: the scorer holds a pointer to the model.
// =============================================================================
#ifndef LPQ_ADAPTATION_ONLINE_SESSION_HPP
#define LPQ_ADAPTATION_ONLINE_SESSION_HPP

#include <cstdint>
#include <utility>

#include "lpq/adaptation/controller_observer.hpp"
#include "lpq/adaptation/online_learner.hpp"
#include "lpq/adaptation/prediction_event.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/lpq/online_node_scorer.hpp"
#include "lpq/models/logistic_predictor.hpp"

namespace lpq {

class OnlineSession final : public FeatureSource {
public:
    using Scorer = OnlineNodeScorer<LogisticPredictor>;

    /// @param features the graph's static features; must outlive the session.
    OnlineSession(const GraphFeatures& features, LogisticPredictor model,
                  OnlineLearningConfig config)
        : features_(features),
          model_(std::move(model)),
          learner_(config),
          scorer_(features_, model_),
          observer_(learner_, *this),
          start_hash_(parameters_fnv1a64(model_)) {}

    OnlineSession(const OnlineSession&)            = delete;
    OnlineSession& operator=(const OnlineSession&) = delete;

    // --- what a search uses -----------------------------------------------------------------

    [[nodiscard]] const Scorer& scorer() const noexcept { return scorer_; }

    /// Attach this to the queue so verified outcomes are recorded.
    [[nodiscard]] LearningObserver& observer() noexcept { return observer_; }

    void features_of(NodeId u, FeatureVector& out) const noexcept override {
        scorer_.features_of(u, out);
    }

    // --- between queries --------------------------------------------------------------------

    /// Applies what the finished search recorded. Returns the number of events applied.
    ///
    /// "Periodic" and "every event" modes apply everything pending (the interval governs only the
    /// opt-in in-search mode). "Adaptive" applies only if the recent error is above its threshold.
    std::size_t end_query() noexcept {
        const UpdateMode mode = learner_.config().mode;
        if (mode == UpdateMode::kDisabled) {
            return 0;
        }
        if (mode == UpdateMode::kAdaptive && !learner_.update_due()) {
            return 0;
        }
        return learner_.flush(model_);
    }

    // --- observation ------------------------------------------------------------------------

    [[nodiscard]] const LogisticPredictor& model() const noexcept { return model_; }
    [[nodiscard]] const OnlineLearner&     learner() const noexcept { return learner_; }
    [[nodiscard]] OnlineLearner&           learner() noexcept { return learner_; }
    [[nodiscard]] const LearningObserver&  observer() const noexcept { return observer_; }

    /// Identity of the parameters at session start and now (FNV-1a over weights and bias).
    [[nodiscard]] std::uint64_t start_hash() const noexcept { return start_hash_; }
    [[nodiscard]] std::uint64_t current_hash() const noexcept { return parameters_fnv1a64(model_); }

    /// True when the parameters differ from where the session started. Measured, not inferred from
    /// the number of updates: an update can leave the weights where they were.
    [[nodiscard]] bool weights_changed() const noexcept { return current_hash() != start_hash_; }

private:
    const GraphFeatures& features_;
    LogisticPredictor    model_;
    OnlineLearner        learner_;
    Scorer               scorer_;
    LearningObserver     observer_;
    std::uint64_t        start_hash_;
};

}  // namespace lpq

#endif  // LPQ_ADAPTATION_ONLINE_SESSION_HPP
