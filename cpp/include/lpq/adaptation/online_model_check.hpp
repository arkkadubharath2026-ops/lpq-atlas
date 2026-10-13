// =============================================================================
// Is this model artifact acceptable as the starting point of an online session? (M5-O4)
//
// Loading a model file successfully says only that its container is well formed (magic, format
// version, feature layout version, feature count and kind, all checked by the loader). It does NOT
// say the model suits this graph. This header adds the checks the loader cannot make, and records
// what was checked and what was overridden:
//
//   * the weights are finite and bounded;
//   * no weight sits on a feature the static scorer does not supply (it would read zero there and
//     the model would be silently mis-scored);
//   * the report that travels with the artifact names the same graph checksum and bucket width the
//     run uses, or the operator said explicitly that it should be accepted anyway;
//   * a model with no report is refused unless the operator accepts "no provenance".
//
// An override is recorded in the result. A model that passes every check is still reported with
// suitability "not assessed": nothing here measures whether the model is any good on this graph.
// =============================================================================
#ifndef LPQ_ADAPTATION_ONLINE_MODEL_CHECK_HPP
#define LPQ_ADAPTATION_ONLINE_MODEL_CHECK_HPP

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lpq/adaptation/online_learner.hpp"
#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/model_report.hpp"

namespace lpq {

class OnlineModelError : public LpqError {
public:
    explicit OnlineModelError(const std::string& what) : LpqError(what) {}
};

struct OnlineModelPolicy {
    bool  allow_graph_mismatch  = false;
    bool  allow_width_mismatch  = false;
    bool  allow_no_provenance   = false;
    float max_abs_weight        = 1000.0F;
};

struct OnlineModelCompat {
    /// "report" when a report was read, "absent_accepted" when none was and the operator allowed it.
    std::string provenance = "report";
    /// "match", "mismatch_overridden" or "unknown" (no report, or no such field in it).
    std::string graph_match = "unknown";
    std::string width_match = "unknown";
    std::vector<std::string> overrides;
    /// Always "not assessed": loading and passing these checks do not measure suitability.
    std::string suitability = "not assessed";
};

/// Indices of the columns the static scorer supplies.
[[nodiscard]] inline std::vector<std::size_t> static_feature_columns() {
    NodeStaticFeatures ones;
    ones.out_degree = ones.min_adjacent_weight = ones.mean_adjacent_weight = 1.0F;
    ones.max_adjacent_weight = ones.weight_spread = ones.degree_relative_to_mean = 1.0F;
    FeatureVector x;
    fill_static_features(ones, x);
    std::vector<std::size_t> cols;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        if (x[i] != 0.0F) {
            cols.push_back(i);
        }
    }
    return cols;
}

/// Throws OnlineModelError if the model is not acceptable; otherwise returns what was checked.
[[nodiscard]] inline OnlineModelCompat check_online_model(
    const LogisticPredictor& model, const std::optional<ModelReport>& report,
    std::uint64_t graph_checksum, std::uint64_t bucket_width, const OnlineModelPolicy& policy) {
    if (!parameters_within(model, policy.max_abs_weight)) {
        throw OnlineModelError(
            "the model has a non-finite weight or one beyond the allowed magnitude (" +
            std::to_string(policy.max_abs_weight) + ")");
    }

    const std::vector<std::size_t> static_cols = static_feature_columns();
    std::vector<bool> is_static(kFeatureCount, false);
    for (const std::size_t i : static_cols) {
        is_static[i] = true;
    }
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        if (!is_static[i] && model.weights()[i] != 0.0F) {
            throw OnlineModelError(
                "the model has a nonzero weight on feature column " + std::to_string(i) +
                ", which the static scorer does not supply; it would be scored as if that "
                "feature were always zero");
        }
    }

    OnlineModelCompat out;
    if (!report) {
        if (!policy.allow_no_provenance) {
            throw OnlineModelError(
                "no model report was given, so the graph and bucket width this model was trained "
                "for cannot be checked; pass --online-model-report, or --online-model-no-provenance "
                "to accept the model unchecked (recorded in the result)");
        }
        out.provenance = "absent_accepted";
        out.overrides.emplace_back("no_provenance");
        return out;
    }

    if (!report->graph_checksum) {
        if (!policy.allow_no_provenance) {
            throw OnlineModelError("the model report has no graph_checksum field");
        }
        out.overrides.emplace_back("no_graph_checksum_in_report");
    } else if (*report->graph_checksum == graph_checksum) {
        out.graph_match = "match";
    } else if (policy.allow_graph_mismatch) {
        out.graph_match = "mismatch_overridden";
        out.overrides.emplace_back("graph_mismatch");
    } else {
        throw OnlineModelError(
            "the model was trained on a different graph (report checksum " +
            std::to_string(*report->graph_checksum) + ", this graph " +
            std::to_string(graph_checksum) +
            "); pass --allow-graph-mismatch to use it anyway (recorded in the result)");
    }

    if (!report->bucket_width) {
        if (!policy.allow_no_provenance) {
            throw OnlineModelError("the model report has no bucket_width field");
        }
        out.overrides.emplace_back("no_bucket_width_in_report");
    } else if (*report->bucket_width == bucket_width) {
        out.width_match = "match";
    } else if (policy.allow_width_mismatch) {
        out.width_match = "mismatch_overridden";
        out.overrides.emplace_back("width_mismatch");
    } else {
        throw OnlineModelError(
            "the model's labels were defined for bucket width " +
            std::to_string(*report->bucket_width) + " but this run uses " +
            std::to_string(bucket_width) +
            "; a different width is a different learning problem. Pass --allow-width-mismatch to "
            "use it anyway (recorded in the result)");
    }
    return out;
}

}  // namespace lpq

#endif  // LPQ_ADAPTATION_ONLINE_MODEL_CHECK_HPP
