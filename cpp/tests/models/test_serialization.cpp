// =============================================================================
// Model serialization (ESD.md section 12, ESD.md section 13).
//
// The dangerous failure mode for a model file is not a crash -- it is loading
// successfully and predicting nonsense. Most of these tests exist to prove that
// the loader REJECTS something, because a check that does not actually reject
// is indistinguishable from no check at all.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/models/heuristic_predictor.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/mlp_predictor.hpp"
#include "lpq/models/predictor.hpp"
#include "lpq/models/serialization.hpp"
#include "lpq/models/tree_ensemble.hpp"

namespace lpq {
namespace {

std::stringstream make_stream() {
    return std::stringstream(std::ios::in | std::ios::out | std::ios::binary);
}

// -----------------------------------------------------------------------------
// Primitive encoding
// -----------------------------------------------------------------------------

TEST(ModelEncoding, WritesIntegersLittleEndianRegardlessOfHostOrder) {
    // The byte order is checked against literal expected bytes rather than
    // against a round-trip. A round-trip would pass on a big-endian host while
    // producing a file that no other machine could read.
    auto os = make_stream();
    detail::put_u32(os, 0x01020304U);
    const std::string bytes = os.str();

    ASSERT_EQ(bytes.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0x04U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 0x03U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[2]), 0x02U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[3]), 0x01U);
}

TEST(ModelEncoding, WritesSixtyFourBitIntegersLittleEndian) {
    auto os = make_stream();
    detail::put_u64(os, 0x0102030405060708ULL);
    const std::string bytes = os.str();

    ASSERT_EQ(bytes.size(), 8U);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]),
                  static_cast<unsigned char>(8 - i))
            << "byte " << i;
    }
}

TEST(ModelEncoding, RoundTripsIntegersAndFloats) {
    auto s = make_stream();
    detail::put_u32(s, 0xDEADBEEFU);
    detail::put_u64(s, 0xFEEDFACECAFEBEEFULL);
    detail::put_f32(s, 3.14159F);
    detail::put_f64(s, 2.718281828459045);
    s.seekg(0);

    EXPECT_EQ(detail::get_u32(s), 0xDEADBEEFU);
    EXPECT_EQ(detail::get_u64(s), 0xFEEDFACECAFEBEEFULL);
    EXPECT_EQ(detail::get_f32(s), 3.14159F);
    EXPECT_EQ(detail::get_f64(s), 2.718281828459045);
}

TEST(ModelEncoding, PreservesFloatBitPatternsExactly) {
    // Exact equality, not a tolerance: a serialization that loses the low bits
    // would change scores near a tie and therefore change the search path.
    const std::vector<float> values = {0.0F,      -0.0F,   1.0F,      -1.0F,
                                       1e-38F,    3.4e38F, 0.1F,      -0.7071068F};
    auto                     s      = make_stream();
    for (float v : values) {
        detail::put_f32(s, v);
    }
    s.seekg(0);
    for (float v : values) {
        const float read = detail::get_f32(s);
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        std::memcpy(&a, &v, sizeof(a));
        std::memcpy(&b, &read, sizeof(b));
        EXPECT_EQ(a, b) << "value " << v;
    }
}

TEST(ModelEncoding, ThrowsOnATruncatedRead) {
    auto s = make_stream();
    s.write("ab", 2);  // half a 32-bit field
    s.seekg(0);
    EXPECT_THROW((void)detail::get_u32(s), ModelSerializationError);
}

TEST(ModelEncoding, RefusesAnImplausibleArrayLength) {
    // The length is read before the data, so an unchecked value would allocate
    // before the read failed -- turning a malformed file into an out-of-memory
    // abort rather than an exception.
    auto s = make_stream();
    detail::put_u64(s, 1ULL << 50);
    s.seekg(0);
    EXPECT_THROW((void)detail::get_f32_array(s, 1024), ModelSerializationError);
}

// -----------------------------------------------------------------------------
// The header
// -----------------------------------------------------------------------------

TEST(ModelHeader, RoundTrips) {
    auto s = make_stream();
    write_model_header(s, ModelKind::kLogistic);
    s.seekg(0);

    const ModelHeader h = read_model_header(s);
    EXPECT_EQ(h.magic, kModelMagic);
    EXPECT_EQ(h.format_version, kModelFormatVersion);
    EXPECT_EQ(h.feature_layout_version, static_cast<std::uint32_t>(kFeatureLayoutVersion));
    EXPECT_EQ(h.feature_count, static_cast<std::uint32_t>(kFeatureCount));
    EXPECT_EQ(h.kind, ModelKind::kLogistic);
}

TEST(ModelHeader, RejectsAFileThatIsNotAModel) {
    auto s = make_stream();
    s << "this is a text file, not a model at all";
    s.seekg(0);
    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

TEST(ModelHeader, RejectsAnEmptyFile) {
    auto s = make_stream();
    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

// The most important rejection in the file. A model trained against one feature
// layout and evaluated against another reads every feature from the wrong
// column and produces confident, plausible, wrong predictions. Nothing
// downstream can detect that, because the outputs are the right shape and in
// the right range.
TEST(ModelHeader, RefusesAModelBuiltAgainstADifferentFeatureLayout) {
    auto s = make_stream();
    detail::put_u64(s, kModelMagic);
    detail::put_u32(s, kModelFormatVersion);
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureLayoutVersion) + 1U);
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureCount));
    detail::put_u32(s, static_cast<std::uint32_t>(ModelKind::kLinear));
    s.seekg(0);

    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

TEST(ModelHeader, RefusesAModelWithADifferentFeatureCount) {
    auto s = make_stream();
    detail::put_u64(s, kModelMagic);
    detail::put_u32(s, kModelFormatVersion);
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureLayoutVersion));
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureCount) + 3U);
    detail::put_u32(s, static_cast<std::uint32_t>(ModelKind::kLinear));
    s.seekg(0);

    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

TEST(ModelHeader, RefusesAnUnknownFormatVersion) {
    auto s = make_stream();
    detail::put_u64(s, kModelMagic);
    detail::put_u32(s, kModelFormatVersion + 99U);
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureLayoutVersion));
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureCount));
    detail::put_u32(s, static_cast<std::uint32_t>(ModelKind::kLinear));
    s.seekg(0);

    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

TEST(ModelHeader, RefusesAnUnknownModelKind) {
    auto s = make_stream();
    detail::put_u64(s, kModelMagic);
    detail::put_u32(s, kModelFormatVersion);
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureLayoutVersion));
    detail::put_u32(s, static_cast<std::uint32_t>(kFeatureCount));
    detail::put_u32(s, 9999U);
    s.seekg(0);

    EXPECT_THROW((void)read_model_header(s), ModelSerializationError);
}

TEST(ModelHeader, RefusesToLoadOneModelKindAsAnother) {
    auto            s = make_stream();
    LogisticPredictor logistic;
    logistic.save(s);
    s.seekg(0);
    EXPECT_THROW((void)LinearPredictor::load(s), ModelSerializationError);
}

TEST(ModelHeader, PeekDoesNotConsumeTheStream) {
    auto s = make_stream();
    MlpPredictor(8).save(s);
    s.seekg(0);

    EXPECT_EQ(peek_model_kind(s), ModelKind::kMlp);
    // The stream must still be readable from the top after peeking, otherwise
    // dispatch-by-kind would need a second open.
    const MlpPredictor loaded = MlpPredictor::load(s);
    EXPECT_EQ(loaded.hidden_units(), 8U);
}

// -----------------------------------------------------------------------------
// Whole-model round trips
//
// ESD.md section 13 step 6 requires exported models to be validated before use.
// Byte-exact score equality is the right bar: a model whose predictions shift
// after a save/load would produce a different search path from the one that was
// evaluated.
// -----------------------------------------------------------------------------

FeatureVector sample_features() {
    FeatureVector x{};
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        x[i] = 0.1F * static_cast<float>(i % 7) - 0.2F;
    }
    return x;
}

TEST(ModelRoundTrip, ConstantPredictorSurvivesExactly) {
    const ConstantPredictor original(2.5F, 0.75F);
    const ConstantPredictor loaded = round_trip(original);

    const FeatureVector x = sample_features();
    EXPECT_EQ(loaded.predict(x).score, original.predict(x).score);
    EXPECT_EQ(loaded.predict(x).confidence, original.predict(x).confidence);
}

TEST(ModelRoundTrip, KeyHeuristicPredictorSurvivesExactly) {
    const KeyHeuristicPredictor original(2.75F);
    const KeyHeuristicPredictor loaded = round_trip(original);

    const FeatureVector x = sample_features();
    EXPECT_EQ(loaded.predict(x).score, original.predict(x).score);
    EXPECT_EQ(loaded.predict(x).confidence, original.predict(x).confidence);
}

TEST(ModelRoundTrip, LinearPredictorSurvivesExactly) {
    LinearPredictor original;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        original.set_weight(static_cast<Feature>(i), 0.37F * static_cast<float>(i) - 1.1F);
    }
    original.set_bias(-0.25F);
    original.set_calibrator(TemperatureScaler(1.75F));

    const LinearPredictor loaded = round_trip(original);
    const FeatureVector   x      = sample_features();

    EXPECT_EQ(loaded.predict(x).score, original.predict(x).score);
    EXPECT_EQ(loaded.predict(x).confidence, original.predict(x).confidence);
    EXPECT_EQ(loaded.bias(), original.bias());
    EXPECT_NEAR(loaded.calibrator().temperature(), 1.75F, 1e-6F);
}

TEST(ModelRoundTrip, LogisticPredictorSurvivesExactlyIncludingUpdateCount) {
    LogisticPredictor original;
    TrainingExample   ex;
    ex.features = sample_features();
    ex.target   = 1.0F;
    for (int i = 0; i < 25; ++i) {
        original.update(ex);
    }

    const LogisticPredictor loaded = round_trip(original);
    EXPECT_EQ(loaded.updates_applied(), 25U);
    EXPECT_EQ(loaded.predict(ex.features).score, original.predict(ex.features).score);
}

TEST(ModelRoundTrip, MlpSurvivesExactly) {
    MlpPredictor original(12);
    Rng          rng(99U);
    original.initialise(rng);

    const MlpPredictor loaded = round_trip(original);
    EXPECT_EQ(loaded.hidden_units(), 12U);

    const FeatureVector x = sample_features();
    EXPECT_EQ(loaded.predict(x).score, original.predict(x).score);
}

TEST(ModelRoundTrip, TrainingConfigurationSurvives) {
    LinearTrainingConfig config;
    config.learning_rate    = 0.05F;
    config.l2               = 0.002F;
    config.positive_weight  = 8.0F;
    config.max_weight_delta = 0.5F;

    const LinearPredictor loaded = round_trip(LinearPredictor(config));
    EXPECT_EQ(loaded.config().learning_rate, 0.05F);
    EXPECT_EQ(loaded.config().l2, 0.002F);
    EXPECT_EQ(loaded.config().positive_weight, 8.0F);
    EXPECT_EQ(loaded.config().max_weight_delta, 0.5F);
}

// -----------------------------------------------------------------------------
// Corrupt payloads
// -----------------------------------------------------------------------------

TEST(ModelLoad, RejectsATruncatedPayload) {
    auto s = make_stream();
    LinearPredictor{}.save(s);

    std::string bytes = s.str();
    bytes.resize(bytes.size() / 2);  // valid header, half a body

    auto truncated = make_stream();
    truncated.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    truncated.seekg(0);

    EXPECT_THROW((void)LinearPredictor::load(truncated), ModelSerializationError);
}

TEST(ModelLoad, RejectsANonFiniteWeight) {
    // A NaN weight would propagate into every score and every comparison, and
    // comparisons against NaN are all false -- so the ordering would silently
    // become arbitrary rather than failing.
    auto s = make_stream();
    write_model_header(s, ModelKind::kLinear);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        detail::put_f32(s, i == 3 ? std::numeric_limits<float>::quiet_NaN() : 0.5F);
    }
    detail::put_f32(s, 0.0F);   // bias
    detail::put_f32(s, 1.0F);   // temperature
    detail::put_f32(s, 0.01F);  // learning rate
    detail::put_f32(s, 0.0F);   // l2
    detail::put_f32(s, 1.0F);   // positive weight
    detail::put_f32(s, 1.0F);   // max weight delta
    detail::put_u64(s, 0U);     // updates
    s.seekg(0);

    EXPECT_THROW((void)LinearPredictor::load(s), ModelSerializationError);
}

TEST(ModelLoad, RejectsAnInvertingTemperature) {
    auto s = make_stream();
    write_model_header(s, ModelKind::kLinear);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        detail::put_f32(s, 0.0F);
    }
    detail::put_f32(s, 0.0F);
    detail::put_f32(s, -2.0F);  // negative temperature
    detail::put_f32(s, 0.01F);
    detail::put_f32(s, 0.0F);
    detail::put_f32(s, 1.0F);
    detail::put_f32(s, 1.0F);
    detail::put_u64(s, 0U);
    s.seekg(0);

    EXPECT_THROW((void)LinearPredictor::load(s), InvalidConfigError);
}

TEST(ModelLoad, RejectsAnMlpWithAnImplausibleHiddenWidth) {
    auto s = make_stream();
    write_model_header(s, ModelKind::kMlp);
    detail::put_u64(s, kMaxHiddenUnits + 1U);
    s.seekg(0);
    EXPECT_THROW((void)MlpPredictor::load(s), ModelSerializationError);
}

TEST(ModelLoad, RejectsAnMlpWhoseArraysDisagreeWithItsDeclaredWidth) {
    auto s = make_stream();
    write_model_header(s, ModelKind::kMlp);
    detail::put_u64(s, 4U);                        // claims 4 hidden units
    detail::put_f32_array(s, std::vector<float>(2 * kFeatureCount, 0.1F));  // supplies 2
    detail::put_f32_array(s, std::vector<float>(4, 0.0F));
    detail::put_f32_array(s, std::vector<float>(4, 0.0F));
    detail::put_f32(s, 0.0F);
    detail::put_f32(s, 1.0F);
    detail::put_f32(s, 0.01F);
    detail::put_f32(s, 0.0F);
    detail::put_f32(s, 1.0F);
    detail::put_f32(s, 1.0F);
    detail::put_u64(s, 0U);
    s.seekg(0);

    EXPECT_THROW((void)MlpPredictor::load(s), ModelSerializationError);
}

// -----------------------------------------------------------------------------
// Files
// -----------------------------------------------------------------------------

TEST(ModelFile, ReportsAnUnwritablePath) {
    EXPECT_THROW(save_model(LinearPredictor{}, "no/such/directory/model.lpq"),
                 ModelSerializationError);
}

TEST(ModelFile, ReportsAMissingPath) {
    EXPECT_THROW((void)load_model<LinearPredictor>("no/such/directory/model.lpq"),
                 ModelSerializationError);
}

}  // namespace
}  // namespace lpq
