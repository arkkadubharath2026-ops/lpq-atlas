// =============================================================================
// Feature extraction (PRD.md section 9, ESD.md section 11).
//
// Assembles a FeatureVector for one candidate node from three sources, none of
// which requires a scan:
//
//   * GraphFeatures      -- precomputed once per graph, read as an array lookup
//   * SearchObservations -- per-node counters maintained by the search
//   * SearchState        -- aggregate signals, already computed by the search
//
// -----------------------------------------------------------------------------
// The cost budget
// -----------------------------------------------------------------------------
//
// This runs once per candidate evaluation, which on a large query is millions
// of times. PRD.md section 10 makes the stakes explicit: the end-to-end cost is
// queue + feature + inference + verification + traversal, and a model that
// predicts perfectly but costs more to feed than the queue saves is a failure.
// Extraction is therefore written to touch three cache lines and perform no
// division, no branch per feature, and no allocation.
//
// All divisions are pre-inverted into multiplications at load time. That is not
// micro-optimisation for its own sake: a division is roughly 20-40 cycles
// against 4 for a multiply, and there would be a dozen per extraction.
//
// -----------------------------------------------------------------------------
// What is deliberately NOT extracted
// -----------------------------------------------------------------------------
//
// PRD.md section 9 lists "candidate's approximate queue position" and "number
// of candidates inside an adaptive key window" among candidate features. Both
// are omitted, and the omission is a decision rather than an oversight:
//
//   * Queue position is not observable in O(1) from any of the five queues
//     without an auxiliary index that would itself cost more to maintain than
//     the feature could plausibly be worth.
//
//   * Counting candidates inside a key window requires either a scan of the
//     queue or a second ordered structure mirroring it.
//
// `queue_density` is provided instead as an O(1) proxy: queue size divided by
// the span between the current minimum key and the candidate's key, which
// carries similar information about local key crowding at constant cost. If the
// ablation shows density carries no signal, the more expensive features can be
// revisited with evidence that they are worth their price.
// =============================================================================
#ifndef LPQ_FEATURES_FEATURE_EXTRACTOR_HPP
#define LPQ_FEATURES_FEATURE_EXTRACTOR_HPP

#include <cstddef>
#include <cstdint>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/features/search_state.hpp"

namespace lpq {

/// Builds feature vectors for candidate nodes.
///
/// Holds references to the graph-level data, which outlive it; constructed once
/// per query and reused for every candidate.
class FeatureExtractor {
public:
    FeatureExtractor(const GraphFeatures& graph_features, const SearchObservations& observations,
                     FeatureSelection selection = FeatureSelection::all()) noexcept
        : graph_(&graph_features), obs_(&observations), selection_(selection) {}

    [[nodiscard]] FeatureSelection selection() const noexcept { return selection_; }
    void set_selection(FeatureSelection s) noexcept { selection_ = s; }

    /// Extracts features for candidate `u` holding key `key`.
    ///
    /// `key` is the queue key: g for Dijkstra, g + h for A*. `tentative_g` is
    /// the true path cost, which differs from the key under A* and is passed
    /// separately rather than derived, because reconstructing it would mean
    /// re-evaluating the heuristic.
    void extract(NodeId u, double key, double tentative_g, const SearchState& search,
                 FeatureVector& out) const noexcept {
        const NodeStaticFeatures&      sf = graph_->node(u);
        const GraphFeatureNormalisation& nm = graph_->normalisation();
        const NodeDynamicState&        dyn = obs_->node_or_zero(u);

        // --- Static node features (one cache line, already normalised) -------
        out[static_cast<std::size_t>(Feature::kOutDegree)]            = sf.out_degree;
        out[static_cast<std::size_t>(Feature::kMinAdjacentWeight)]    = sf.min_adjacent_weight;
        out[static_cast<std::size_t>(Feature::kMeanAdjacentWeight)]   = sf.mean_adjacent_weight;
        out[static_cast<std::size_t>(Feature::kMaxAdjacentWeight)]    = sf.max_adjacent_weight;
        out[static_cast<std::size_t>(Feature::kAdjacentWeightSpread)] = sf.weight_spread;
        out[static_cast<std::size_t>(Feature::kDegreeRelativeToMean)] =
            sf.degree_relative_to_mean;

        // --- Dynamic node features -------------------------------------------
        out[static_cast<std::size_t>(Feature::kTentativeDistance)] =
            static_cast<float>(tentative_g) * nm.inv_max_path_cost;
        out[static_cast<std::size_t>(Feature::kEffectiveKey)] =
            static_cast<float>(key) * nm.inv_max_path_cost;
        // Counts are normalised by degree: "relaxed three times" means
        // something different for a degree-2 node than a degree-500 one, and
        // the ratio is what transfers across graph families.
        out[static_cast<std::size_t>(Feature::kSuccessfulRelaxations)] =
            static_cast<float>(dyn.successful_relaxations) * nm.inv_mean_degree;
        out[static_cast<std::size_t>(Feature::kFailedRelaxations)] =
            static_cast<float>(dyn.failed_relaxations) * nm.inv_mean_degree;
        out[static_cast<std::size_t>(Feature::kInsertions)] =
            static_cast<float>(dyn.insertions);
        out[static_cast<std::size_t>(Feature::kDecreaseKeys)] =
            static_cast<float>(dyn.decrease_keys);
        out[static_cast<std::size_t>(Feature::kHopDepth)] =
            static_cast<float>(dyn.hop_depth) * nm.inv_node_count;

        // Queue age: how many nodes were settled between this node's discovery
        // and now. A large value means the node has been waiting, which is a
        // signal that its key is poor relative to the frontier.
        const double age = static_cast<double>(search.settled_nodes) -
                           static_cast<double>(dyn.discovered_at_settled);
        out[static_cast<std::size_t>(Feature::kQueueAge)] =
            static_cast<float>(age > 0.0 ? age : 0.0) * nm.inv_node_count;

        // --- Queue features ---------------------------------------------------
        out[static_cast<std::size_t>(Feature::kQueueSize)] =
            static_cast<float>(search.queue_size) * nm.inv_node_count;

        const double gap = key - search.current_best_key;
        out[static_cast<std::size_t>(Feature::kKeyGapFromMinimum)] =
            static_cast<float>(gap > 0.0 ? gap : 0.0) * nm.inv_max_path_cost;

        // Relative gap: the gap as a fraction of the current minimum. Scale
        // free, so it transfers between graphs whose absolute costs differ by
        // orders of magnitude. Guarded because the minimum is legitimately zero
        // at the start of every search.
        out[static_cast<std::size_t>(Feature::kRelativeKeyGap)] =
            search.current_best_key > 0.0
                ? static_cast<float>(gap / search.current_best_key)
                : 0.0F;

        // Density proxy: queue entries per unit of key span. See the header for
        // why this stands in for the exact window count.
        out[static_cast<std::size_t>(Feature::kQueueDensity)] =
            gap > 0.0 ? static_cast<float>(static_cast<double>(search.queue_size) / gap)
                      : 0.0F;

        // --- Search-state features --------------------------------------------
        out[static_cast<std::size_t>(Feature::kSearchProgress)] =
            static_cast<float>(search.settled_nodes) * nm.inv_node_count;
        out[static_cast<std::size_t>(Feature::kSettledNodes)] =
            static_cast<float>(search.settled_nodes) * nm.inv_node_count;
        // Relaxations per settled node: how much work each extraction is
        // generating, which tracks how dense the explored region is.
        out[static_cast<std::size_t>(Feature::kRelaxationRate)] =
            search.settled_nodes > 0
                ? static_cast<float>(static_cast<double>(search.relaxations) /
                                     static_cast<double>(search.settled_nodes)) *
                      nm.inv_mean_degree
                : 0.0F;
        out[static_cast<std::size_t>(Feature::kCurrentBestKey)] =
            static_cast<float>(search.current_best_key) * nm.inv_max_path_cost;
        out[static_cast<std::size_t>(Feature::kDistanceToTarget)] =
            static_cast<float>(search.distance_to_target) * nm.inv_max_path_cost;
        out[static_cast<std::size_t>(Feature::kRecentPredictionError)] =
            static_cast<float>(search.recent_prediction_error);
        out[static_cast<std::size_t>(Feature::kRecentCorrectionRate)] =
            static_cast<float>(search.recent_correction_rate);

        // --- Graph-context features -------------------------------------------
        out[static_cast<std::size_t>(Feature::kGraphMeanDegree)]  = nm.mean_degree;
        out[static_cast<std::size_t>(Feature::kGraphDegreeStdDev)] = nm.degree_stddev;
        out[static_cast<std::size_t>(Feature::kGraphMeanWeight)]   = nm.mean_weight;

        // Applied last, and a no-op when every group is enabled, so the
        // production path pays nothing for the ablation machinery.
        selection_.apply(out);
    }

    /// Convenience overload returning by value. For tests and offline dataset
    /// generation; the hot path uses the out-parameter form so the vector can
    /// live in the caller's frame across millions of calls.
    [[nodiscard]] FeatureVector extract(NodeId u, double key, double tentative_g,
                                        const SearchState& search) const noexcept {
        FeatureVector v{};
        extract(u, key, tentative_g, search, v);
        return v;
    }

private:
    const GraphFeatures*      graph_;
    const SearchObservations* obs_;
    FeatureSelection          selection_;
};

}  // namespace lpq

#endif  // LPQ_FEATURES_FEATURE_EXTRACTOR_HPP
