// =============================================================================
// Reading a model report and deciding whether a model may start an online session (M5-O4).
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

#include "lpq/adaptation/online_model_check.hpp"
#include "lpq/models/model_report.hpp"

namespace lpq {
namespace {

// The shape python/lpq_tools/training/train.py writes (json.dumps, indent=2).
const char* kReport = R"({
  "dataset_id": "snap_facebook",
  "graph_checksum": 27545307309881865,
  "bucket_width": 8,
  "query_count": 200,
  "query_seed": 917,
  "nodes": 4039,
  "roc_auc": 0.798469,
  "seed": 20260923,
  "weights": {"out_degree": 1.5, "min_adjacent_weight": -0.25},
  "bias": -4.26,
  "training_manifest": {
    "name": "snap_facebook_training_queries_source_disjoint.yaml",
    "sha256": "e0105cbe771fdb313960aff3b6d99085335d9a2a615a9515d71d582d78843c1e",
    "selection": "uniform_pairs_distinct_endpoints_source_disjoint",
    "seed": 917,
    "query_count": 200
  }
})";

TEST(ModelReport, ReadsTheProvenanceFieldsTheEngineNeeds) {
    const ModelReport r = parse_model_report(kReport);
    EXPECT_EQ(r.dataset_id, "snap_facebook");
    EXPECT_EQ(*r.graph_checksum, 27545307309881865ULL);
    EXPECT_EQ(*r.bucket_width, 8U);
    EXPECT_EQ(*r.query_seed, 917U);
    EXPECT_EQ(*r.query_count, 200U);
    EXPECT_EQ(*r.training_seed, 20260923U) << "the top-level seed, not the manifest's";
    EXPECT_EQ(r.manifest_name, "snap_facebook_training_queries_source_disjoint.yaml");
    EXPECT_EQ(r.manifest_sha256.size(), 64U);
    EXPECT_EQ(r.manifest_selection, "uniform_pairs_distinct_endpoints_source_disjoint");
}

TEST(ModelReport, ALargeChecksumSurvivesAs64Bits) {
    const ModelReport r = parse_model_report(R"({"graph_checksum": 15161404696693572585})");
    EXPECT_EQ(*r.graph_checksum, 15161404696693572585ULL);
    EXPECT_FALSE(parse_model_report(R"({"graph_checksum": 18446744073709551616})").graph_checksum)
        << "2^64 does not fit and is not silently wrapped";
}

TEST(ModelReport, AReportFromBeforeTheManifestRecordStillParses) {
    const ModelReport r = parse_model_report(R"({"dataset_id":"g","graph_checksum":7,"bucket_width":3,
        "training_manifest": null})");
    EXPECT_EQ(r.dataset_id, "g");
    EXPECT_TRUE(r.manifest_name.empty());
    const ModelReport older = parse_model_report(R"({"dataset_id":"g","graph_checksum":7})");
    EXPECT_FALSE(older.bucket_width);
}

TEST(ModelReport, MalformedJsonIsRejectedNotGuessedAt) {
    for (const char* bad : {"", "{", R"({"a": )", R"({"a": "unterminated})", R"({"a": 1} trailing)",
                            R"({"a" 1})", R"({"a": "bad \q escape"})", "[1, 2"}) {
        EXPECT_THROW((void)parse_model_report(bad), ModelReportError) << bad;
    }
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "[";
    EXPECT_THROW((void)parse_model_report(deep), ModelReportError);
}

LogisticPredictor valid_model() {
    LogisticPredictor m;
    m.set_weight(Feature::kOutDegree, 1.5F);
    m.set_weight(Feature::kMinAdjacentWeight, -0.25F);
    m.set_bias(-4.0F);
    return m;
}

constexpr std::uint64_t kChecksum = 27545307309881865ULL;

TEST(OnlineModelCheck, AMatchingModelPassesAndStillReportsSuitabilityAsNotAssessed) {
    const auto compat = check_online_model(valid_model(), parse_model_report(kReport), kChecksum, 8, {});
    EXPECT_EQ(compat.graph_match, "match");
    EXPECT_EQ(compat.width_match, "match");
    EXPECT_TRUE(compat.overrides.empty());
    EXPECT_EQ(compat.suitability, "not assessed");
}

TEST(OnlineModelCheck, ANonStaticWeightIsRefused) {
    LogisticPredictor m = valid_model();
    m.set_weight(Feature::kQueueSize, 0.1F);  // a dynamic feature the static scorer never supplies
    EXPECT_THROW((void)check_online_model(m, parse_model_report(kReport), kChecksum, 8, {}), OnlineModelError);
}

TEST(OnlineModelCheck, NonFiniteOrAbsurdWeightsAreRefused) {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), 5000.0F}) {
        LogisticPredictor m = valid_model();
        m.set_weight(Feature::kOutDegree, bad);
        EXPECT_THROW((void)check_online_model(m, parse_model_report(kReport), kChecksum, 8, {}), OnlineModelError);
    }
}

TEST(OnlineModelCheck, AGraphMismatchIsRefusedUnlessOverriddenAndTheOverrideIsRecorded) {
    const ModelReport r = parse_model_report(kReport);
    EXPECT_THROW((void)check_online_model(valid_model(), r, kChecksum + 1, 8, {}), OnlineModelError);
    OnlineModelPolicy p;
    p.allow_graph_mismatch = true;
    const auto compat      = check_online_model(valid_model(), r, kChecksum + 1, 8, p);
    EXPECT_EQ(compat.graph_match, "mismatch_overridden");
    ASSERT_EQ(compat.overrides.size(), 1U);
    EXPECT_EQ(compat.overrides[0], "graph_mismatch");
    EXPECT_EQ(compat.suitability, "not assessed") << "an override is never a statement of suitability";
}

TEST(OnlineModelCheck, ABucketWidthMismatchIsADifferentLearningProblemAndNeedsItsOwnOverride) {
    const ModelReport r = parse_model_report(kReport);
    EXPECT_THROW((void)check_online_model(valid_model(), r, kChecksum, 9, {}), OnlineModelError);
    OnlineModelPolicy graph_only;
    graph_only.allow_graph_mismatch = true;
    EXPECT_THROW((void)check_online_model(valid_model(), r, kChecksum, 9, graph_only), OnlineModelError)
        << "the graph override does not cover the width";
    OnlineModelPolicy p;
    p.allow_width_mismatch = true;
    EXPECT_EQ(check_online_model(valid_model(), r, kChecksum, 9, p).width_match, "mismatch_overridden");
}

TEST(OnlineModelCheck, NoReportIsRefusedUnlessProvenanceIsExplicitlyWaived) {
    EXPECT_THROW((void)check_online_model(valid_model(), std::nullopt, kChecksum, 8, {}), OnlineModelError);
    OnlineModelPolicy p;
    p.allow_no_provenance = true;
    const auto compat     = check_online_model(valid_model(), std::nullopt, kChecksum, 8, p);
    EXPECT_EQ(compat.provenance, "absent_accepted");
    EXPECT_EQ(compat.graph_match, "unknown");
    EXPECT_EQ(compat.overrides, std::vector<std::string>{"no_provenance"});
}

TEST(OnlineModelCheck, AReportMissingTheFieldsItIsCheckedAgainstIsRefused) {
    EXPECT_THROW((void)check_online_model(valid_model(), parse_model_report(R"({"dataset_id":"g"})"),
                                           kChecksum, 8, {}), OnlineModelError);
}

TEST(OnlineModelCheck, TheStaticColumnsAreExactlyTheSixTheScorerSupplies) {
    EXPECT_EQ(static_feature_columns().size(), 6U);
}

}  // namespace
}  // namespace lpq
