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
#include <sstream>
#include <string>

#include "lpq/common/error.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/serialization.hpp"

namespace lpq {
namespace {

[[noreturn]] void usage_and_exit(int code) {
    std::cout << "lpq_model_info -- inspect a model file\n\n"
                 "  --model <file>   the model to read (required)\n"
                 "  --dataset <file> with --nodes: a converted graph to score nodes on\n"
                 "  --nodes <a,b,..> print each node's static features and the score the\n"
                 "                   learned queue's StaticNodeScorer gives it (logistic)\n";
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

/// Features and scores exactly as lpq_run computes them: the same GraphFeatures
/// and the same StaticNodeScorer::build, read back per node. There is
/// deliberately no second implementation of either.
void print_node_scores(const LogisticPredictor& model, const std::string& dataset,
                       const std::string& node_list) {
    using W            = std::uint32_t;
    const auto  loaded = read_binary_graph_file<W>(dataset);
    const auto& g      = loaded.graph;
    const auto  meta   = compute_metadata(g);
    const GraphFeatures                 features(g, meta);
    StaticNodeScorer<LogisticPredictor> scorer;
    scorer.build(features, model);

    std::stringstream ss(node_list);
    std::string       item;
    while (std::getline(ss, item, ',')) {
        if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos) {
            throw InvalidConfigError("--nodes expects comma-separated node ids");
        }
        const auto u = static_cast<NodeId>(std::stoull(item));
        if (u >= g.num_nodes()) {
            throw InvalidConfigError("node " + item + " is not in the graph");
        }
        const NodeStaticFeatures& f = features.node(u);
        const auto p = [u](const char* name, float v) {
            std::printf("node.%u.%s: %.9g\n", static_cast<unsigned>(u), name,
                        static_cast<double>(v));
        };
        p("out_degree", f.out_degree);
        p("min_adjacent_weight", f.min_adjacent_weight);
        p("mean_adjacent_weight", f.mean_adjacent_weight);
        p("max_adjacent_weight", f.max_adjacent_weight);
        p("adjacent_weight_spread", f.weight_spread);
        p("degree_relative_to_mean", f.degree_relative_to_mean);
        p("score", scorer.score_of(u));
    }
}

int run(int argc, char** argv) {
    std::string path;
    std::string dataset;
    std::string nodes;
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
        } else if (a == "--dataset" && i + 1 < argc) {
            dataset = argv[++i];
        } else if (a == "--nodes" && i + 1 < argc) {
            nodes = argv[++i];
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
        case ModelKind::kLogistic: {
            const LogisticPredictor model = LogisticPredictor::load(in);
            print_linear(model);
            if (!dataset.empty()) {
                print_node_scores(model, dataset, nodes);
            }
            break;
        }
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
