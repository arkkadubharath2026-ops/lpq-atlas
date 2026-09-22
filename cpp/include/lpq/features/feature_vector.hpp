// =============================================================================
// The feature vector (PRD.md section 9, ESD.md section 11).
//
// ESD.md section 11: "Use a fixed-size numeric feature vector to minimize
// dynamic allocation." This is that vector: a std::array with a compile-time
// size and a fixed index per feature, so extraction writes into a stack object
// and the whole thing fits in a few cache lines.
//
// -----------------------------------------------------------------------------
// float, not double
// -----------------------------------------------------------------------------
//
// Features are float. This is a deliberate departure from the rest of the
// engine, which uses double for anything affecting a path cost. The reasoning
// is that features never affect correctness: they feed a predictor whose output
// is a hint, and every hint is verified before it can change a result. What
// they do affect is speed, and halving the vector's footprint halves the memory
// traffic of the hottest ML path and doubles the width of any vectorised dot
// product. A float carries about seven significant digits, which is far more
// resolution than a normalised feature needs.
//
// -----------------------------------------------------------------------------
// Fixed layout
// -----------------------------------------------------------------------------
//
// Each feature has a permanent index. Adding one appends at the end and bumps
// the layout version; indices are never reordered or reused, because a model
// trained against one layout and evaluated against another would silently read
// the wrong column -- producing plausible, wrong predictions rather than an
// error. The layout version is serialised with every exported model and checked
// on load.
//
// -----------------------------------------------------------------------------
// Normalisation
// -----------------------------------------------------------------------------
//
// Raw features span wildly different scales: a degree is single digits, a
// tentative distance on a road network is millions. A linear or logistic model
// on unnormalised inputs is dominated by whichever feature happens to be
// largest, and gradient updates become ill-conditioned. Features are therefore
// normalised against graph-level statistics computed once at load time, so the
// normalisation itself costs nothing per extraction.
//
// Normalisation constants come only from the GRAPH, never from the query set or
// from any outcome, which is what keeps ESD.md section 30 (no data leakage)
// satisfiable: the same constants are available before a test query runs.
// =============================================================================
#ifndef LPQ_FEATURES_FEATURE_VECTOR_HPP
#define LPQ_FEATURES_FEATURE_VECTOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lpq {

/// Incremented whenever the feature layout changes. Serialised with models.
inline constexpr int kFeatureLayoutVersion = 1;

/// Index of each feature. Never reorder; append only.
enum class Feature : std::uint8_t {
    // --- Static node features (precomputed once per graph) ------------------
    kOutDegree = 0,
    kMinAdjacentWeight,
    kMeanAdjacentWeight,
    kMaxAdjacentWeight,
    kAdjacentWeightSpread,
    kDegreeRelativeToMean,

    // --- Dynamic node features (updated during the search) ------------------
    kTentativeDistance,
    kEffectiveKey,
    kSuccessfulRelaxations,
    kFailedRelaxations,
    kInsertions,
    kDecreaseKeys,
    kHopDepth,
    kQueueAge,

    // --- Queue features -----------------------------------------------------
    kQueueSize,
    kKeyGapFromMinimum,
    kRelativeKeyGap,
    kQueueDensity,

    // --- Search-state features ---------------------------------------------
    kSearchProgress,
    kSettledNodes,
    kRelaxationRate,
    kCurrentBestKey,
    kDistanceToTarget,
    kRecentPredictionError,
    kRecentCorrectionRate,

    // --- Graph-context features --------------------------------------------
    kGraphMeanDegree,
    kGraphDegreeStdDev,
    kGraphMeanWeight,

    /// Not a feature; the count. Must remain last.
    kCount,
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::kCount);

/// Fixed-size feature vector. No allocation, no indirection.
using FeatureVector = std::array<float, kFeatureCount>;

/// Writes a feature by its enum index.
inline void set_feature(FeatureVector& v, Feature f, float value) noexcept {
    v[static_cast<std::size_t>(f)] = value;
}

/// Reads a feature by its enum index.
[[nodiscard]] inline float get_feature(const FeatureVector& v, Feature f) noexcept {
    return v[static_cast<std::size_t>(f)];
}

/// Stable names, used for training-data column headers, model metadata and
/// ablation configuration. Order matches the enum exactly.
[[nodiscard]] inline constexpr std::string_view feature_name(Feature f) noexcept {
    switch (f) {
        case Feature::kOutDegree:             return "out_degree";
        case Feature::kMinAdjacentWeight:     return "min_adjacent_weight";
        case Feature::kMeanAdjacentWeight:    return "mean_adjacent_weight";
        case Feature::kMaxAdjacentWeight:     return "max_adjacent_weight";
        case Feature::kAdjacentWeightSpread:  return "adjacent_weight_spread";
        case Feature::kDegreeRelativeToMean:  return "degree_relative_to_mean";
        case Feature::kTentativeDistance:     return "tentative_distance";
        case Feature::kEffectiveKey:          return "effective_key";
        case Feature::kSuccessfulRelaxations: return "successful_relaxations";
        case Feature::kFailedRelaxations:     return "failed_relaxations";
        case Feature::kInsertions:            return "insertions";
        case Feature::kDecreaseKeys:          return "decrease_keys";
        case Feature::kHopDepth:              return "hop_depth";
        case Feature::kQueueAge:              return "queue_age";
        case Feature::kQueueSize:             return "queue_size";
        case Feature::kKeyGapFromMinimum:     return "key_gap_from_minimum";
        case Feature::kRelativeKeyGap:        return "relative_key_gap";
        case Feature::kQueueDensity:          return "queue_density";
        case Feature::kSearchProgress:        return "search_progress";
        case Feature::kSettledNodes:          return "settled_nodes";
        case Feature::kRelaxationRate:        return "relaxation_rate";
        case Feature::kCurrentBestKey:        return "current_best_key";
        case Feature::kDistanceToTarget:      return "distance_to_target";
        case Feature::kRecentPredictionError: return "recent_prediction_error";
        case Feature::kRecentCorrectionRate:  return "recent_correction_rate";
        case Feature::kGraphMeanDegree:       return "graph_mean_degree";
        case Feature::kGraphDegreeStdDev:     return "graph_degree_stddev";
        case Feature::kGraphMeanWeight:       return "graph_mean_weight";
        case Feature::kCount:                 return "<count>";
    }
    return "<unknown>";
}

/// Feature groups, for the ablation study (PRD.md section 21, item 10:
/// "different feature subsets").
///
/// Groups rather than individual features, because the interesting ablation
/// question is which KIND of information carries the signal -- static graph
/// structure, dynamic search state, queue geometry -- not whether one
/// particular column matters.
enum class FeatureGroup : std::uint8_t {
    kStaticNode = 0,
    kDynamicNode,
    kQueue,
    kSearchState,
    kGraphContext,
    kCount,
};

inline constexpr std::size_t kFeatureGroupCount = static_cast<std::size_t>(FeatureGroup::kCount);

[[nodiscard]] inline constexpr std::string_view feature_group_name(FeatureGroup gr) noexcept {
    switch (gr) {
        case FeatureGroup::kStaticNode:   return "static_node";
        case FeatureGroup::kDynamicNode:  return "dynamic_node";
        case FeatureGroup::kQueue:        return "queue";
        case FeatureGroup::kSearchState:  return "search_state";
        case FeatureGroup::kGraphContext: return "graph_context";
        case FeatureGroup::kCount:        return "<count>";
    }
    return "<unknown>";
}

/// The group a feature belongs to.
[[nodiscard]] inline constexpr FeatureGroup group_of(Feature f) noexcept {
    switch (f) {
        case Feature::kOutDegree:
        case Feature::kMinAdjacentWeight:
        case Feature::kMeanAdjacentWeight:
        case Feature::kMaxAdjacentWeight:
        case Feature::kAdjacentWeightSpread:
        case Feature::kDegreeRelativeToMean:
            return FeatureGroup::kStaticNode;

        case Feature::kTentativeDistance:
        case Feature::kEffectiveKey:
        case Feature::kSuccessfulRelaxations:
        case Feature::kFailedRelaxations:
        case Feature::kInsertions:
        case Feature::kDecreaseKeys:
        case Feature::kHopDepth:
        case Feature::kQueueAge:
            return FeatureGroup::kDynamicNode;

        case Feature::kQueueSize:
        case Feature::kKeyGapFromMinimum:
        case Feature::kRelativeKeyGap:
        case Feature::kQueueDensity:
            return FeatureGroup::kQueue;

        case Feature::kSearchProgress:
        case Feature::kSettledNodes:
        case Feature::kRelaxationRate:
        case Feature::kCurrentBestKey:
        case Feature::kDistanceToTarget:
        case Feature::kRecentPredictionError:
        case Feature::kRecentCorrectionRate:
            return FeatureGroup::kSearchState;

        case Feature::kGraphMeanDegree:
        case Feature::kGraphDegreeStdDev:
        case Feature::kGraphMeanWeight:
            return FeatureGroup::kGraphContext;

        case Feature::kCount:
            break;
    }
    return FeatureGroup::kCount;
}

/// Which feature groups an experiment enables.
///
/// Held as a bitmask so the check is a single test, and applied by ZEROING
/// disabled features after extraction rather than by branching during it.
/// Branching per feature would make the ablation configuration change the
/// extraction cost, so a feature subset would appear faster partly because it
/// skipped work and partly because it took different branches -- confounding
/// the very comparison the ablation exists to make. Zeroing keeps extraction
/// cost identical across subsets, so a runtime difference is attributable to
/// the model rather than to the extractor.
class FeatureSelection {
public:
    /// All groups enabled.
    constexpr FeatureSelection() noexcept : mask_((1U << kFeatureGroupCount) - 1U) {}

    explicit constexpr FeatureSelection(std::uint32_t mask) noexcept : mask_(mask) {}

    [[nodiscard]] static constexpr FeatureSelection all() noexcept { return {}; }
    [[nodiscard]] static constexpr FeatureSelection none() noexcept {
        return FeatureSelection(0U);
    }

    constexpr FeatureSelection& enable(FeatureGroup gr) noexcept {
        mask_ |= (1U << static_cast<std::uint32_t>(gr));
        return *this;
    }

    constexpr FeatureSelection& disable(FeatureGroup gr) noexcept {
        mask_ &= ~(1U << static_cast<std::uint32_t>(gr));
        return *this;
    }

    [[nodiscard]] constexpr bool enabled(FeatureGroup gr) const noexcept {
        return (mask_ & (1U << static_cast<std::uint32_t>(gr))) != 0U;
    }

    [[nodiscard]] constexpr bool all_enabled() const noexcept {
        return mask_ == ((1U << kFeatureGroupCount) - 1U);
    }

    [[nodiscard]] constexpr std::uint32_t mask() const noexcept { return mask_; }

    /// Zeroes every feature belonging to a disabled group.
    void apply(FeatureVector& v) const noexcept {
        if (all_enabled()) {
            return;  // the common case: no work at all
        }
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            if (!enabled(group_of(static_cast<Feature>(i)))) {
                v[i] = 0.0F;
            }
        }
    }

private:
    std::uint32_t mask_;
};

}  // namespace lpq

#endif  // LPQ_FEATURES_FEATURE_VECTOR_HPP
