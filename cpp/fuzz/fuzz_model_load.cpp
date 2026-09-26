// Fuzz target: model deserialisation (LPQ Atlas spec D-07).
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "lpq/features/feature_vector.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/serialization.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    try {
        std::istringstream probe(bytes);
        (void)lpq::peek_model_kind(probe);
        std::istringstream     in(bytes);
        const auto             model = lpq::LogisticPredictor::load(in);
        const lpq::FeatureVector x{};
        const float            s = model.predict(x).score;
        (void)s;
    } catch (const lpq::LpqError&) {
        // Rejected cleanly.
    }
    return 0;
}
