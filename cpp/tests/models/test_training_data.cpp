// =============================================================================
// Tests for the training-set container (ESD.md section 39 milestone 15).
//
// The round trip is the least interesting thing here. What matters is that a
// set which does not describe this build is REFUSED rather than read, because
// every way of getting that wrong is silent:
//
//   * a file that is not a training set, read as one, yields plausible garbage;
//   * a set built against another feature layout puts every value in the wrong
//     column and trains a confident, wrong model;
//   * a row claiming more wins than appearances is a labelling bug upstream,
//     and it would train without complaint.
//
// None of those produce a crash or an obviously wrong number, so the only place
// they can be caught is at the boundary.
// =============================================================================
#include <gtest/gtest.h>

#include <sstream>

#include "lpq/models/training_data.hpp"

namespace lpq {
namespace {

TrainingSet make_set(std::uint32_t positives = 3, std::uint32_t occurrences = 10) {
    TrainingSet set;
    set.dataset_id     = "dimacs_ny";
    set.graph_checksum = 0xDEADBEEFCAFEF00DULL;
    set.bucket_width   = 37;
    set.query_count    = 100;
    set.query_seed     = 42;

    TrainingRow a;
    set_feature(a.features, Feature::kOutDegree, 0.25F);
    set_feature(a.features, Feature::kMeanAdjacentWeight, 0.5F);
    a.positives   = positives;
    a.occurrences = occurrences;

    TrainingRow b;
    set_feature(b.features, Feature::kOutDegree, 0.75F);
    b.positives   = 0;
    b.occurrences = 4;

    set.rows = {a, b};
    return set;
}

TrainingSet round_trip(const TrainingSet& in) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_training_set(buffer, in);
    return read_training_set(buffer);
}

// -----------------------------------------------------------------------------
// Round trip
// -----------------------------------------------------------------------------

TEST(TrainingData, PreservesProvenanceThroughARoundTrip) {
    const TrainingSet out = round_trip(make_set());

    // Every one of these is required to interpret the labels later. The bucket
    // width especially: "first out of its bucket" is a different question at
    // every width, so a set without it is unusable.
    EXPECT_EQ(out.dataset_id, "dimacs_ny");
    EXPECT_EQ(out.graph_checksum, 0xDEADBEEFCAFEF00DULL);
    EXPECT_EQ(out.bucket_width, 37U);
    EXPECT_EQ(out.query_count, 100U);
    EXPECT_EQ(out.query_seed, 42U);
}

TEST(TrainingData, PreservesFeaturesExactly) {
    // Bit-exact, not approximately. The values are written as bits rather than
    // formatted precisely so that a training set which is written and read back
    // trains to the same model.
    const TrainingSet out = round_trip(make_set());

    ASSERT_EQ(out.rows.size(), 2U);
    EXPECT_FLOAT_EQ(out.rows[0].features[static_cast<std::size_t>(Feature::kOutDegree)], 0.25F);
    EXPECT_FLOAT_EQ(
        out.rows[0].features[static_cast<std::size_t>(Feature::kMeanAdjacentWeight)], 0.5F);
    EXPECT_FLOAT_EQ(out.rows[1].features[static_cast<std::size_t>(Feature::kOutDegree)], 0.75F);
}

TEST(TrainingData, KeepsNeverWonDistinctFromNeverAppeared) {
    // Two different observations. A reader that conflated them would be fitting
    // a model to data that does not exist.
    const TrainingSet out = round_trip(make_set());

    ASSERT_EQ(out.rows.size(), 2U);
    EXPECT_EQ(out.rows[1].positives, 0U);
    EXPECT_EQ(out.rows[1].occurrences, 4U);
}

TEST(TrainingData, AggregatesReportTheImbalanceATrainerNeeds) {
    const TrainingSet set = make_set();

    EXPECT_EQ(set.total_positives(), 3U);
    EXPECT_EQ(set.total_occurrences(), 14U);
}

TEST(TrainingData, SurvivesAnEmptySet) {
    // Legal, and reachable: a query set where nothing is reachable from any
    // source produces one. It must read back as empty rather than as an error
    // or as garbage.
    TrainingSet set;
    set.dataset_id = "empty";
    const TrainingSet out = round_trip(set);

    EXPECT_TRUE(out.rows.empty());
    EXPECT_EQ(out.total_occurrences(), 0U);
}

// -----------------------------------------------------------------------------
// Refusals
// -----------------------------------------------------------------------------

TEST(TrainingData, RefusesAFileThatIsNotATrainingSet) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    buffer << "this is not a training set, it is a text file";

    EXPECT_THROW({ [[maybe_unused]] const auto s = read_training_set(buffer); },
                 TrainingSetError);
}

TEST(TrainingData, RefusesARowClaimingMoreWinsThanAppearances) {
    // Impossible by construction, so seeing it means the collector mislabelled.
    // Caught here because such a set trains perfectly well and silently.
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_training_set(buffer, make_set(/*positives=*/11, /*occurrences=*/10));

    EXPECT_THROW({ [[maybe_unused]] const auto s = read_training_set(buffer); },
                 TrainingSetError);
}

TEST(TrainingData, RefusesATruncatedFile) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_training_set(buffer, make_set());

    std::string bytes = buffer.str();
    bytes.resize(bytes.size() / 2);
    std::stringstream truncated(bytes, std::ios::in | std::ios::binary);

    EXPECT_THROW({ [[maybe_unused]] const auto s = read_training_set(truncated); },
                 TrainingSetError);
}

TEST(TrainingData, RefusesAnUnreadableFormatVersion) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_training_set(buffer, make_set());

    // Bump the format version in place: magic is 8 bytes, the version follows.
    std::string bytes = buffer.str();
    bytes[8]          = static_cast<char>(kTrainingSetFormatVersion + 1);
    std::stringstream future(bytes, std::ios::in | std::ios::binary);

    EXPECT_THROW({ [[maybe_unused]] const auto s = read_training_set(future); },
                 TrainingSetError);
}

TEST(TrainingData, RefusesAMismatchedFeatureLayout) {
    // The dangerous one. A set built against another layout reads every feature
    // from the wrong column and produces a model that is confident, plausible
    // and wrong, which nothing downstream can detect.
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_training_set(buffer, make_set());

    std::string bytes = buffer.str();
    bytes[12]         = static_cast<char>(kFeatureLayoutVersion + 1);
    std::stringstream other(bytes, std::ios::in | std::ios::binary);

    EXPECT_THROW({ [[maybe_unused]] const auto s = read_training_set(other); },
                 TrainingSetError);
}

TEST(TrainingData, RefusesAMissingFile) {
    EXPECT_THROW({ [[maybe_unused]] const auto s =
                       read_training_set_file("does-not-exist.lpqtrain"); },
                 TrainingSetError);
}

}  // namespace
}  // namespace lpq
