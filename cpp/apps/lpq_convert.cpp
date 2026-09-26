// =============================================================================
// lpq_convert -- the convert, metadata and checksum steps of the dataset
// pipeline (ESD.md section 26).
//
//     download -> validate -> normalize -> CONVERT TO CSR -> COMPUTE METADATA
//     -> generate query manifest -> STORE CHECKSUM
//
// The capitalised steps live here; the rest live in python/lpq_tools/data/.
// The split is not arbitrary. Conversion has to touch every arc of a graph that
// may hold tens of millions of them, and the checksum is a sequential hash over
// that whole payload -- neither is work for Python. Acquisition and manifest
// generation are I/O-bound orchestration, which is not work for C++.
//
// The deciding argument, though, is that the binary format has exactly one
// implementation. A second writer in Python would have to reproduce the field
// order, the little-endian encoding, the canonical intra-adjacency order and
// the FNV-1a checksum, and the two would drift. Here Python decides WHAT to
// convert and this tool decides HOW, so the format is defined in one place.
//
// Usage:
//   lpq_convert --input datasets/raw/USA-road-d.NY.gr --format dimacs
//               --coordinates datasets/raw/USA-road-d.NY.co
//               --output datasets/processed/osm_ny.lpqgraph
//               --metadata datasets/metadata/osm_ny.json
//               --dataset-id osm_ny
//
// Nothing about any particular dataset is compiled in: a new graph is a new
// config file, not a new build.
// =============================================================================
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/graph/weight_distributions.hpp"
#include "lpq/metrics/json_writer.hpp"

namespace {

using namespace lpq;

/// The project's default weight type. DIMACS travel times and distances for the
/// full USA graph stay well inside 32 bits, and a narrower weight halves the
/// arc array, which is the dominant cache footprint of the search.
using Weight = std::uint32_t;

enum class SourceFormat { kSnap, kDimacs, kBinary };

struct Options {
    std::string  input;
    std::string  coordinates;
    std::string  output;
    std::string  metadata;
    std::string  dataset_id;
    SourceFormat format = SourceFormat::kSnap;

    // Normalization policy (ESD.md section 26's "normalize" step). Defaults
    // match the readers' own defaults so that omitting a flag changes nothing.
    Directedness       directedness   = Directedness::kUndirected;
    SelfLoopPolicy     self_loops     = SelfLoopPolicy::kDrop;
    ParallelEdgePolicy parallel_edges = ParallelEdgePolicy::kKeep;
    std::uint64_t      default_weight = 1;
    bool               edges_listed_both = false;

    // Synthetic weights, for datasets distributed without them.
    WeightDistributionConfig weights;
    bool                     synthesize = false;
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "lpq_convert: " << message << "\n";
    std::exit(2);
}

void print_usage() {
    std::cout <<
        "Usage: lpq_convert --input PATH --format snap|dimacs|binary --output PATH\n"
        "                   [--metadata PATH] [--coordinates PATH] [--dataset-id ID]\n"
        "                   [--directedness directed|undirected]\n"
        "                   [--self-loops keep|drop] [--parallel-edges keep|keep_minimum]\n"
        "                   [--default-weight N]\n"
        "                   [--weights constant|uniform|lognormal|exponential|\n"
        "                              narrow_range|heavy_tailed]\n"
        "                   [--weight-min X] [--weight-max X] [--weight-seed N]\n"
        "                   [--weight-sigma X] [--weight-alpha X] [--narrow-fraction X]\n";
}

[[nodiscard]] std::string require_value(int argc, char** argv, int& i, std::string_view flag) {
    if (i + 1 >= argc) {
        fail(std::string(flag) + " requires a value");
    }
    return argv[++i];
}

[[nodiscard]] double require_double(int argc, char** argv, int& i, std::string_view flag) {
    const std::string raw = require_value(argc, argv, i, flag);
    try {
        std::size_t consumed = 0;
        const double v = std::stod(raw, &consumed);
        if (consumed != raw.size()) {
            fail(std::string(flag) + ": '" + raw + "' is not a number");
        }
        return v;
    } catch (const std::exception&) {
        fail(std::string(flag) + ": '" + raw + "' is not a number");
    }
}

[[nodiscard]] std::uint64_t require_u64(int argc, char** argv, int& i, std::string_view flag) {
    const std::string raw = require_value(argc, argv, i, flag);
    try {
        std::size_t consumed = 0;
        const unsigned long long v = std::stoull(raw, &consumed);
        if (consumed != raw.size()) {
            fail(std::string(flag) + ": '" + raw + "' is not a non-negative integer");
        }
        return static_cast<std::uint64_t>(v);
    } catch (const std::exception&) {
        fail(std::string(flag) + ": '" + raw + "' is not a non-negative integer");
    }
}

Options parse_arguments(int argc, char** argv) {
    Options o;
    bool weight_bounds_seen = false;
    bool directedness_seen  = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (arg == "--input") {
            o.input = require_value(argc, argv, i, arg);
        } else if (arg == "--coordinates") {
            o.coordinates = require_value(argc, argv, i, arg);
        } else if (arg == "--output") {
            o.output = require_value(argc, argv, i, arg);
        } else if (arg == "--metadata") {
            o.metadata = require_value(argc, argv, i, arg);
        } else if (arg == "--dataset-id") {
            o.dataset_id = require_value(argc, argv, i, arg);
        } else if (arg == "--format") {
            const std::string v = require_value(argc, argv, i, arg);
            if (v == "snap") {
                o.format = SourceFormat::kSnap;
            } else if (v == "dimacs") {
                o.format = SourceFormat::kDimacs;
            } else if (v == "binary") {
                o.format = SourceFormat::kBinary;
            } else {
                fail("unknown --format '" + v + "' (expected snap, dimacs or binary)");
            }
        } else if (arg == "--directedness") {
            directedness_seen   = true;
            const std::string v = require_value(argc, argv, i, arg);
            if (v == "directed") {
                o.directedness = Directedness::kDirected;
            } else if (v == "undirected") {
                o.directedness = Directedness::kUndirected;
            } else {
                fail("unknown --directedness '" + v + "'");
            }
        } else if (arg == "--self-loops") {
            const std::string v = require_value(argc, argv, i, arg);
            if (v == "keep") {
                o.self_loops = SelfLoopPolicy::kKeep;
            } else if (v == "drop") {
                o.self_loops = SelfLoopPolicy::kDrop;
            } else {
                fail("unknown --self-loops '" + v + "'");
            }
        } else if (arg == "--parallel-edges") {
            const std::string v = require_value(argc, argv, i, arg);
            if (v == "keep") {
                o.parallel_edges = ParallelEdgePolicy::kKeep;
            } else if (v == "keep_minimum") {
                o.parallel_edges = ParallelEdgePolicy::kKeepMinimum;
            } else {
                fail("unknown --parallel-edges '" + v + "'");
            }
        } else if (arg == "--edges-listed") {
            const std::string v = require_value(argc, argv, i, arg);
            if (v == "once") {
                o.edges_listed_both = false;
            } else if (v == "both") {
                o.edges_listed_both = true;
            } else {
                fail("unknown --edges-listed '" + v + "' (expected once or both)");
            }
        } else if (arg == "--default-weight") {
            o.default_weight = require_u64(argc, argv, i, arg);
        } else if (arg == "--weights") {
            const std::string v = require_value(argc, argv, i, arg);
            if (!parse_weight_distribution(v, o.weights.kind)) {
                fail("unknown --weights '" + v + "'");
            }
            o.synthesize = true;
        } else if (arg == "--weight-min") {
            o.weights.minimum  = require_double(argc, argv, i, arg);
            weight_bounds_seen = true;
        } else if (arg == "--weight-max") {
            o.weights.maximum  = require_double(argc, argv, i, arg);
            weight_bounds_seen = true;
        } else if (arg == "--weight-seed") {
            o.weights.seed = require_u64(argc, argv, i, arg);
        } else if (arg == "--weight-sigma") {
            o.weights.sigma = require_double(argc, argv, i, arg);
        } else if (arg == "--weight-alpha") {
            o.weights.alpha = require_double(argc, argv, i, arg);
        } else if (arg == "--narrow-fraction") {
            o.weights.narrow_fraction = require_double(argc, argv, i, arg);
        } else {
            fail("unrecognised argument '" + std::string(arg) + "'");
        }
    }

    // DIMACS files list each direction as its own arc record, and a binary
    // file already holds materialised arcs, so neither is pairing anything up.
    // Reporting them as undirected -- the SNAP-shaped default -- would halve
    // their edge count in the metadata.
    if (!directedness_seen && o.format != SourceFormat::kSnap) {
        o.directedness = Directedness::kDirected;
    }

    if (o.input.empty()) {
        fail("--input is required");
    }
    if (o.output.empty()) {
        fail("--output is required");
    }
    if (o.dataset_id.empty()) {
        o.dataset_id = std::filesystem::path(o.output).stem().string();
    }
    if (o.synthesize && !weight_bounds_seen) {
        // A synthesis request with no range would silently produce the default
        // constant range [1, 1], i.e. quietly ignore the requested family.
        fail("--weights requires --weight-min and --weight-max");
    }
    if (!o.synthesize && weight_bounds_seen) {
        fail("--weight-min/--weight-max have no effect without --weights");
    }
    return o;
}

[[nodiscard]] std::ifstream open_input(const std::string& path, bool binary) {
    std::ifstream is(path, binary ? std::ios::binary : std::ios::in);
    if (!is) {
        fail("cannot open '" + path + "' for reading");
    }
    return is;
}

[[nodiscard]] LoadedGraph<Weight> load(const Options& o) {
    switch (o.format) {
        case SourceFormat::kSnap: {
            std::ifstream is = open_input(o.input, false);
            SnapReadOptions snap;
            snap.default_weight = o.default_weight;
            snap.directedness   = o.directedness;
            snap.self_loops     = o.self_loops;
            snap.parallel_edges = o.parallel_edges;
            snap.edges_listed_both_directions = o.edges_listed_both;
            return read_snap_edge_list<Weight>(is, snap);
        }
        case SourceFormat::kDimacs: {
            std::ifstream is = open_input(o.input, false);
            return read_dimacs_graph<Weight>(is);
        }
        case SourceFormat::kBinary: {
            std::ifstream is = open_input(o.input, true);
            return read_binary_graph<Weight>(is);
        }
    }
    fail("unreachable: unhandled source format");
}

/// Writes the metadata ESD.md section 5 requires, plus the provenance that
/// makes a processed graph traceable back to its source.
void write_metadata(const std::string& path, const Options& o, const GraphMetadata& m,
                    const LoadedGraph<Weight>& loaded) {
    std::ofstream os(path);
    if (!os) {
        fail("cannot open '" + path + "' for writing");
    }

    JsonWriter w(os);
    w.begin_object();

    w.key_value("dataset_id", o.dataset_id);
    w.key_value("source_path", o.input);
    w.key_value("source_format", o.format == SourceFormat::kSnap     ? "snap"
                                 : o.format == SourceFormat::kDimacs ? "dimacs"
                                                                     : "binary");
    w.key_value("processed_path", o.output);

    // ESD.md section 26's "store checksum" step. Recorded here as well as in
    // the file header so a metadata record identifies exactly one graph.
    w.key_value("graph_checksum_fnv1a64", static_cast<std::uint64_t>(loaded.checksum));
    w.key_value("weight_type", "uint32");

    w.key_value("node_count", static_cast<std::uint64_t>(m.node_count));
    w.key_value("edge_count", static_cast<std::uint64_t>(m.edge_count));
    w.key_value("arc_count", static_cast<std::uint64_t>(m.arc_count));
    w.key_value("directed", o.directedness == Directedness::kDirected);
    w.key_value("density", m.density());

    w.key_value("min_weight", m.min_weight);
    w.key_value("max_weight", m.max_weight);
    w.key_value("mean_weight", m.mean_weight);
    w.key_value("stddev_weight", m.stddev_weight);
    w.key_value("max_path_cost_bound", m.max_path_cost_bound());

    w.key_value("min_out_degree", static_cast<std::uint64_t>(m.min_out_degree));
    w.key_value("max_out_degree", static_cast<std::uint64_t>(m.max_out_degree));
    w.key_value("mean_out_degree", m.mean_out_degree);
    w.key_value("stddev_out_degree", m.stddev_out_degree);
    w.key_value("isolated_nodes", static_cast<std::uint64_t>(m.isolated_nodes));

    w.key_value("has_coordinates", !loaded.coordinates.empty());
    w.key_value("has_node_id_mapping", !loaded.mapping.empty());

    w.begin_object("normalization");
    w.key_value("directedness",
                o.directedness == Directedness::kDirected ? "directed" : "undirected");
    w.key_value("self_loops", o.self_loops == SelfLoopPolicy::kKeep ? "keep" : "drop");
    w.key_value("parallel_edges",
                o.parallel_edges == ParallelEdgePolicy::kKeep ? "keep" : "keep_minimum");
    w.key_value("edges_listed", o.edges_listed_both ? "both" : "once");
    w.end_object();

    // The weights of a SNAP graph are not a property of the dataset -- they are
    // a property of this conversion. Recording the family and the seed is what
    // makes the distinction visible downstream; without it a results table
    // would attribute a synthetic weight distribution to the source.
    w.begin_object("weights");
    w.key_value("synthesized", o.synthesize);
    if (o.synthesize) {
        w.key_value("distribution", std::string(weight_distribution_name(o.weights.kind)));
        w.key_value("minimum", o.weights.minimum);
        w.key_value("maximum", o.weights.maximum);
        w.key_value("seed", static_cast<std::uint64_t>(o.weights.seed));
        if (o.weights.kind == WeightDistribution::kLogNormal) {
            w.key_value("sigma", o.weights.sigma);
        }
        if (o.weights.kind == WeightDistribution::kHeavyTailed) {
            w.key_value("alpha", o.weights.alpha);
        }
        if (o.weights.kind == WeightDistribution::kNarrowRange) {
            w.key_value("narrow_fraction", o.weights.narrow_fraction);
        }
    } else {
        w.key_value("source", "dataset");
    }
    w.end_object();

    w.end_object();
    os << "\n";

    if (!os) {
        fail("failed while writing metadata to '" + path + "'");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options o = parse_arguments(argc, argv);

        LoadedGraph<Weight> loaded = load(o);

        if (!o.coordinates.empty()) {
            std::ifstream is = open_input(o.coordinates, false);
            read_dimacs_coordinates(is, loaded.graph.num_nodes(), loaded.coordinates);
        }

        if (o.synthesize) {
            o.weights.validate();
            loaded.graph = synthesize_weights<Weight>(loaded.graph, o.weights,
                                                      loaded.coordinates,
                                                      CoordinateSystem::kGeographic);
            loaded.checksum = graph_checksum(loaded.graph);
        }

        // DIMACS .co files carry microdegrees, but the file labels these
        // coordinates kGeographic, whose contract -- and HaversineHeuristic's --
        // is DEGREES. Stored unconverted, the Haversine heuristic evaluated
        // trigonometry on values in the millions: its derived scale stayed
        // admissible but collapsed towards zero, silently turning A* into
        // Dijkstra. Converted here, after any weight synthesis, so synthesized
        // datasets keep exactly the weights they were built with. The arc
        // checksum does not cover coordinates and is unchanged.
        if (!o.coordinates.empty()) {
            for (Coordinate& c : loaded.coordinates) {
                c.x *= 1e-6;
                c.y *= 1e-6;
            }
        }

        const std::filesystem::path out_path(o.output);
        if (out_path.has_parent_path()) {
            std::filesystem::create_directories(out_path.parent_path());
        }
        // DIMACS .co files carry longitude/latitude, so the system is
        // geographic. It has to be stated here: the coordinates are read into a
        // bare array, and the graph the converter loaded does not carry a
        // system of its own.
        write_binary_graph_file<Weight>(o.output, loaded.graph, loaded.mapping,
                                        loaded.coordinates,
                                        loaded.coordinates.empty()
                                            ? CoordinateSystem::kNone
                                            : CoordinateSystem::kGeographic);

        GraphMetadata m = compute_metadata(loaded.graph);
        m.dataset_id    = o.dataset_id;

        if (!o.metadata.empty()) {
            const std::filesystem::path meta_path(o.metadata);
            if (meta_path.has_parent_path()) {
                std::filesystem::create_directories(meta_path.parent_path());
            }
            write_metadata(o.metadata, o, m, loaded);
        }

        std::cout << o.dataset_id << ": " << m.node_count << " nodes, " << m.arc_count
                  << " arcs, checksum 0x" << std::hex << loaded.checksum << std::dec << "\n";
        return 0;
    } catch (const LpqError& e) {
        std::cerr << "lpq_convert: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "lpq_convert: " << e.what() << "\n";
        return 1;
    }
}
