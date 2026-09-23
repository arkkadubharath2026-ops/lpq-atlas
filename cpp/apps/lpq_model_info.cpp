// =============================================================================
// lpq_model_info -- reads a model file and prints what the engine sees in it.
//
//   lpq_model_info --model models/exported/dimacs_ny_static.lpqmodel
//
// Two jobs. The obvious one is debugging: a model is otherwise an opaque blob,
// and "which file is this and what does it think" is a question worth being
// able to answer without a debugger.
//
// The one that earns its place in the build is cross-language verification. The
// training pipeline writes these files from Python, using a second
// implementation of a binary layout whose authority is serialization.hpp. Two
// implementations of a format are a liability unless something checks they
// agree, and agreement can only be established by loading a Python-written file
// with the REAL C++ loader and comparing what comes back. That is what this
// tool makes possible, and what python/tests/test_model_io.py does with it.
//
// Output is line-oriented `key: value` so a test can parse it without a JSON
// dependency in a tool whose whole purpose is to have no interesting
// dependencies.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {
namespace {

[[noreturn]] void usage_and_exit(int code) {
    std::cout << "lpq_model_info -- inspect a model file\n\n"
                 "  --model <file>   the model to read (required)\n";
    std::exit(code);
}

/// Prints the parameter block of whichever linear-family model this is.
///
/// Weights are printed to nine significant digits. They are stored as float32,
/// and nine digits is the shortest decimal that round-trips every float32
/// exactly -- fewer would make a comparison against the writer's value fail for
/// reasons of formatting rather than of content.
template <typename Model>
void print_linear(const Model& model) {
    const FeatureVector& w = model.weights();
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        std::printf("weight.%s: %.9g\n",
                    std::string(feature_name(static_cast<Feature>(i))).c_str(),
                    static_cast<double>(w[i]));
    }
    std::printf("bias: %.9g\n", static_cast<double>(model.bias()));
}

int run(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage_and_exit(0);
        }
        if (a == "--model") {
            if (i + 1 >= argc) {
                throw InvalidConfigError("--model requires a value");
            }
            path = argv[++i];
        } else {
            throw InvalidConfigError("unknown argument: " + a);
        }
    }
    if (path.empty()) {
        throw InvalidConfigError("--model is required");
    }

    // The kind is read from the file's own claim rather than guessed, then the
    // stream is rewound so the matching loader sees a complete file. Dispatching
    // on the caller's expectation instead would defeat the point of the tag.
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        throw ModelSerializationError("cannot open model: " + path);
    }
    const ModelKind kind = peek_model_kind(probe);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ModelSerializationError("cannot reopen model: " + path);
    }

    std::printf("kind: %s\n", model_kind_name(kind));
    std::printf("feature_layout_version: %u\n",
                static_cast<unsigned>(kFeatureLayoutVersion));
    std::printf("feature_count: %u\n", static_cast<unsigned>(kFeatureCount));

    switch (kind) {
        case ModelKind::kLinear:
            print_linear(LinearPredictor::load(in));
            break;
        case ModelKind::kLogistic:
            print_linear(LogisticPredictor::load(in));
            break;
        default:
            // Not an error: the file is valid and this tool simply does not
            // print that family yet. Saying so is better than pretending the
            // file is bad.
            std::printf("detail: this tool prints parameters only for the linear family\n");
            break;
    }
    return 0;
}

}  // namespace
}  // namespace lpq

int main(int argc, char** argv) {
    try {
        return lpq::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
