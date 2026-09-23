// =============================================================================
// Tests for experiment manifest reading (ESD.md sections 21 and 22).
//
// The parsing tests matter less than the refusal tests. A manifest that fails
// to parse is obvious and gets fixed in seconds. A manifest that parses into
// something slightly different from what it says produces a full results file,
// correctly formatted, carrying the manifest's name and somebody else's
// numbers -- and nothing downstream can detect it.
//
// So the cases with the most attention here are the ones where accepting the
// input would be worse than rejecting it: a mode the runner cannot honour, a
// duplicated key, a list written as a scalar, a repetition count too small for
// the dispersion statistic this project reports.
// =============================================================================
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "lpq/metrics/experiment_manifest.hpp"

namespace lpq {
namespace {

/// A manifest in the shape ESD.md section 22 specifies.
std::string manifest_text(const std::string& extra = "",
                          const std::string& modes = "mode:\n  - exact\n") {
    return "experiment: ny_exact_v1\n"
           "dataset: dimacs_ny\n"
           "algorithm:\n"
           "  - dijkstra\n"
           "pq:\n"
           "  - binary_heap\n"
           "  - lpq\n" +
           modes +
           "query_count: 100\n"
           "repetitions: 11\n"
           "seed: 42\n" +
           extra;
}

ExperimentManifest parse(const std::string& text) {
    std::istringstream is(text);
    return read_experiment_manifest(is);
}

// -----------------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------------

TEST(ExperimentManifest, ParsesTheShapeTheSpecificationGives) {
    const ExperimentManifest m = parse(manifest_text());

    EXPECT_EQ(m.experiment, "ny_exact_v1");
    EXPECT_EQ(m.dataset, "dimacs_ny");
    ASSERT_EQ(m.algorithms.size(), 1U);
    EXPECT_EQ(m.algorithms[0], "dijkstra");
    ASSERT_EQ(m.pqs.size(), 2U);
    EXPECT_EQ(m.pqs[1], "lpq");
    EXPECT_EQ(m.query_count, 100U);
    EXPECT_EQ(m.repetitions, 11U);
    EXPECT_EQ(m.seed, 42U);
}

TEST(ExperimentManifest, ExpandsTheCrossProductInAPredictableOrder) {
    // The order is part of the contract: someone reading the manifest should be
    // able to predict the order of the rows in the results file.
    const ExperimentManifest m = parse(
        "experiment: e\ndataset: d\n"
        "algorithm:\n  - dijkstra\n  - astar\n"
        "pq:\n  - binary_heap\n  - lpq\n"
        "mode:\n  - exact\n"
        "query_count: 10\nrepetitions: 3\nseed: 1\n");

    const auto runs = m.runs();
    ASSERT_EQ(runs.size(), 4U);
    EXPECT_EQ(runs[0].algorithm, "dijkstra");
    EXPECT_EQ(runs[0].pq, "binary_heap");
    EXPECT_EQ(runs[1].pq, "lpq");
    EXPECT_EQ(runs[2].algorithm, "astar");
    EXPECT_EQ(runs[2].pq, "binary_heap");
    EXPECT_EQ(runs[3].algorithm, "astar");
    EXPECT_EQ(runs[3].pq, "lpq");
}

TEST(ExperimentManifest, ReadsTheCollectionBlock) {
    const ExperimentManifest m = parse(manifest_text(
        "collect:\n"
        "  runtime: true\n"
        "  pq_operations: true\n"
        "  ml_latency: false\n"
        "  hardware_counters: true\n"));

    EXPECT_TRUE(m.collect.runtime);
    EXPECT_TRUE(m.collect.pq_operations);
    EXPECT_FALSE(m.collect.ml_latency);
    EXPECT_TRUE(m.collect.hardware_counters);
}

TEST(ExperimentManifest, DefaultsHostDependentCollectionToOff) {
    // A manifest that says nothing about hardware counters is not promising
    // them. The portable subset is the default so that omitting the block
    // cannot produce a record claiming a measurement the host cannot make.
    const ExperimentManifest m = parse(manifest_text());

    EXPECT_TRUE(m.collect.runtime);
    EXPECT_FALSE(m.collect.hardware_counters);
    EXPECT_FALSE(m.collect.ml_latency);
}

TEST(ExperimentManifest, IgnoresCommentsAndBlankLines) {
    const ExperimentManifest m = parse(
        "# the NY exact matrix\n"
        "\n"
        "experiment: e   # short name\n"
        "dataset: d\n"
        "algorithm:\n  - dijkstra\n"
        "pq:\n  - lpq\n"
        "mode:\n  - exact\n"
        "\n"
        "query_count: 10\nrepetitions: 3\nseed: 1\n");

    EXPECT_EQ(m.experiment, "e");
    ASSERT_EQ(m.runs().size(), 1U);
}

// -----------------------------------------------------------------------------
// Refusals that protect the results
// -----------------------------------------------------------------------------

TEST(ExperimentManifest, AcceptsApproximateModeWhenItCarriesASlack) {
    const ExperimentManifest m =
        parse(manifest_text("epsilon: 0.1\n", "mode:\n  - approximate\n"));

    ASSERT_EQ(m.modes.size(), 1U);
    EXPECT_EQ(m.modes[0], "approximate");
    // Kept as text so the recorded value matches the file exactly. Parsing 0.1
    // to a double and printing it back can yield 0.10000000000000001, and an
    // experiment whose recorded epsilon differs from its manifest cannot be
    // matched up afterwards.
    EXPECT_EQ(m.epsilon, "0.1");
}

TEST(ExperimentManifest, RefusesApproximateModeWithNoSlack) {
    // Approximate mode with epsilon unset is exact mode reported under a weaker
    // guarantee: every record would say "approximate" and every number would be
    // exact, and nothing downstream could tell.
    EXPECT_THROW(parse(manifest_text("", "mode:\n  - approximate\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RefusesASlackThatNoModeWouldUse) {
    // The mirror image: an epsilon with no approximate mode would be silently
    // ignored, leaving a manifest that claims an approximation it never applied.
    EXPECT_THROW(parse(manifest_text("epsilon: 0.1\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RefusalsForApproximateModeExplainThemselves) {
    // A bare "unknown mode" would read as a typo and invite someone to try
    // other spellings. The message has to say what is actually wrong.
    try {
        [[maybe_unused]] const auto m = parse(manifest_text("", "mode:\n  - approximate\n"));
        FAIL() << "expected a refusal";
    } catch (const ExperimentManifestError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("epsilon"), std::string::npos);
    }
}

TEST(ExperimentManifest, RejectsAnUnknownQueueRatherThanSkippingIt) {
    // A skipped queue silently shrinks the matrix, and the results file looks
    // complete because every row in it did run.
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n  - binary_heap\n  - splay_heap\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAnUnknownAlgorithm) {
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - bellman_ford\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsADuplicatedKey) {
    // Last-value-wins would mean the file says one seed and the records carry
    // another, which is unreproducible in the most confusing possible way.
    EXPECT_THROW(parse(manifest_text("seed: 7\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAnUnknownTopLevelKey) {
    EXPECT_THROW(parse(manifest_text("optimisation_level: 3\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAnUnknownKeyInsideCollect) {
    EXPECT_THROW(parse(manifest_text("collect:\n  cache_misses: true\n")),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsANonBooleanCollectionFlag) {
    // "yes" would otherwise have to default to something, and defaulting it to
    // false turns a requested measurement off without saying so.
    EXPECT_THROW(parse(manifest_text("collect:\n  runtime: yes\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAListKeyWrittenAsAScalar) {
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm: dijkstra\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAListItemWithNoListAboveIt) {
    EXPECT_THROW(parse("experiment: e\n  - stray\n"), ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsTooFewRepetitionsForADispersionEstimate) {
    // This project reports median and MAD. A MAD from one or two samples is a
    // number with no meaning, and producing it would be worse than refusing.
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 2\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAManifestMissingItsName) {
    EXPECT_THROW(parse("dataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAManifestWithAnEmptyQueueList) {
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n"
                       "mode:\n  - exact\n"
                       "query_count: 10\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAZeroQueryCount) {
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: 0\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsANonNumericCount) {
    EXPECT_THROW(parse("experiment: e\ndataset: d\n"
                       "algorithm:\n  - dijkstra\n"
                       "pq:\n  - lpq\n"
                       "mode:\n  - exact\n"
                       "query_count: many\nrepetitions: 3\nseed: 1\n"),
                 ExperimentManifestError);
}

// -----------------------------------------------------------------------------
// Learned-queue geometry
// -----------------------------------------------------------------------------

TEST(ExperimentManifest, ReadsThePinnedQueueGeometry) {
    const ExperimentManifest m = parse(
        manifest_text("lpq_window_min: 2\nlpq_window_max: 2\nlpq_bucket_width: 25\n"));

    EXPECT_EQ(m.lpq_window_min, 2U);
    EXPECT_EQ(m.lpq_window_max, 2U);
    EXPECT_EQ(m.lpq_bucket_width, 25U);
}

TEST(ExperimentManifest, LeavesTheGeometryAtZeroWhenTheManifestDoesNotPinIt) {
    // Zero is the runner's signal to keep its own defaults. It must not be
    // confused with a window of size zero, which would disable the window.
    const ExperimentManifest m = parse(manifest_text());

    EXPECT_EQ(m.lpq_window_min, 0U);
    EXPECT_EQ(m.lpq_window_max, 0U);
    EXPECT_EQ(m.lpq_bucket_width, 0U);
}

TEST(ExperimentManifest, RejectsHalfAWindow) {
    // One bound from the manifest and one from the runner default produces a
    // geometry that appears in no file, which is the one thing a manifest
    // exists to prevent.
    EXPECT_THROW(parse(manifest_text("lpq_window_min: 2\n")), ExperimentManifestError);
    EXPECT_THROW(parse(manifest_text("lpq_window_max: 8\n")), ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAnInvertedWindow) {
    EXPECT_THROW(parse(manifest_text("lpq_window_min: 8\nlpq_window_max: 2\n")),
                 ExperimentManifestError);
}

TEST(ExperimentManifest, RejectsAMissingFileRatherThanRunningEmpty) {
    EXPECT_THROW({ [[maybe_unused]] const auto m =
                       read_experiment_manifest_file("does-not-exist.yaml"); },
                 ExperimentManifestError);
}

}  // namespace
}  // namespace lpq
