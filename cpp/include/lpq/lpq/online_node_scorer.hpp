// =============================================================================
// A node scorer that reads the model's CURRENT parameters (M5-O3).
//
// StaticNodeScorer evaluates the model once per node and keeps the result, so changing the model
// afterwards changes nothing. This scorer keeps no scores. Each call builds the node's static
// feature vector and takes the dot product with the live weights, so:
//
//   * there is no cached score that could go stale, and
//   * a parameter update costs O(parameters), never O(nodes).
//
// The price is six multiply-adds per push where StaticNodeScorer pays one array read. That cost is
// measured separately and is not claimed to be small or large here.
//
// Bit-for-bit agreement with StaticNodeScorer
// -------------------------------------------------------------------------------------------
// The feature vector comes from the same fill_static_features() and the score from the same
// detail::dot(weights, x) + bias that LogisticPredictor::predict() computes, so for a model that is
// not being updated the two scorers return identical floats. A test compares every node. That
// agreement is the first half of "a frozen lpq_online is lpq_model".
//
// Staleness that remains, and why it is harmless
// -------------------------------------------------------------------------------------------
// The substrate stores the score an entry had when it was pushed. If the model is updated while a
// search is running (an opt-in mode, not the default), entries queued earlier keep their older
// score. A score only chooses the ORDER in which entries are examined; verification compares keys.
// So such an entry costs comparisons at worst and can never make an answer wrong.
//
// A non-finite score is replaced by zero. Keys remain authoritative either way.
// =============================================================================
#ifndef LPQ_LPQ_ONLINE_NODE_SCORER_HPP
#define LPQ_LPQ_ONLINE_NODE_SCORER_HPP

#include <cmath>
#include <cstddef>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/models/linear_predictor.hpp"

namespace lpq {

/// @tparam Model a LinearPredictor-derived model (weights() and bias()).
template <typename Model>
class OnlineNodeScorer {
public:
    OnlineNodeScorer() = default;

    /// Both referents must outlive the scorer. The model may change between (or, in the opt-in
    /// mode, during) searches; the scorer always reads its current parameters.
    OnlineNodeScorer(const GraphFeatures& features, const Model& model) noexcept
        : features_(&features), model_(&model) {}

    [[nodiscard]] float score_of(NodeId u) const noexcept {
        if (features_ == nullptr || model_ == nullptr || u >= features_->size()) {
            return 0.0F;
        }
        FeatureVector x;
        fill_static_features(features_->node(u), x);
        const float score = detail::dot(model_->weights(), x) + model_->bias();
        return std::isfinite(score) ? score : 0.0F;
    }

    /// The feature vector this scorer uses for a node (the learner trains on the same vector).
    void features_of(NodeId u, FeatureVector& out) const noexcept {
        if (features_ == nullptr || u >= features_->size()) {
            out.fill(0.0F);
            return;
        }
        fill_static_features(features_->node(u), out);
    }

    [[nodiscard]] std::size_t size() const noexcept { return features_ ? features_->size() : 0; }

private:
    const GraphFeatures* features_ = nullptr;
    const Model*         model_    = nullptr;
};

}  // namespace lpq

#endif  // LPQ_LPQ_ONLINE_NODE_SCORER_HPP
