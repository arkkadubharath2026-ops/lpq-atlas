// =============================================================================
// lpq_reweight -- derive a graph with the SAME topology as an existing
// .lpqgraph but a DIFFERENT edge-weight distribution.
//
// Purpose. A learned queue-configuration policy trained and validated on six
// real graphs has six data points on the axis it most needs to generalise
// over: the weight distribution. Holding topology (and coordinates) fixed while
// varying only the weights is a controlled way to add points on that axis.
//
// What this tool is NOT. Its output is a SYNTHETIC-WEIGHT graph. It is not a
// real road network and must never be reported as one. Every derived dataset
// is labelled derived_* with `weights.synthesized: true` and a
// `derived_from` / `transform` / `seed` record by python/lpq_tools/data/derive.py,
// which is the intended way to invoke this tool.
//
// Usage:
//   lpq_reweight --input in.lpqgraph --output out.lpqgraph
//                --transform <spec> --seed N [--stats-json PATH]
//
// Transforms (every one yields integer weights in [1, 2^31 - 1]):
//   unit                      every weight 1
//   uniform:LO:HI             uniform integer in [LO, HI]
//   scale:K                   w * K              (K integer >= 1)
//   clip:M                    min(w, M)          (M integer >= 1)
//   lognormal:MEDIAN:SIGMA    round(MEDIAN * exp(SIGMA * N(0,1)))
//   pareto:XMIN:ALPHA         round(XMIN * (1 - U)^(-1/ALPHA)), U in [0,1)
//   bimodal:SMALL:LARGE:P     LARGE with probability P, otherwise SMALL
//
// Determinism. Draws come from lpq::Rng seeded with
// derive_seed(seed, "lpq_reweight/" + spec), so the same (input, spec, seed)
// yields a byte-identical output on any platform, and two specs sharing a seed
// do not share a stream. `unit`, `scale` and `clip` use no randomness; the seed
// is accepted (and recorded by the caller) but has no effect on them.
//
// Symmetry. If the input holds the reverse arc v->u for an arc u->v, both
// receive the SAME weight (one draw per pair). Real road graphs, and every
// undirected graph, are symmetric; independent per-arc draws would silently
// turn them into asymmetric digraphs. Arcs with no partner are drawn alone.
// Parallel arcs are paired in adjacency order.
//
// Clamping. A draw outside [1, 2^31 - 1] is clamped and COUNTED (per draw; a
// symmetric arc pair is one draw); both counts are printed and written to the
// stats file. Nothing is clamped silently.
//
// Coordinates and the node-id mapping are carried over unchanged.
//
// A* CAVEAT. Reweighting does not change coordinates, so any coordinate-based
// heuristic scale chosen for the ORIGINAL weights is no longer valid. This
// engine derives that scale from the graph it is given
// (euclidean_admissible_scale / haversine_admissible_scale: min over arcs of
// weight / geometric length), so loading a reweighted graph recomputes it and
// A* stays admissible and consistent -- but the scale, and with it the
// heuristic's strength, collapses when weights are not proportional to length.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/metrics/json_writer.hpp"

namespace {

using namespace lpq;
using Weight = std::uint32_t;

/// Largest weight a derived graph may carry. Well inside uint32, and small
/// enough that (n - 1) * max_weight cannot approach the 64-bit distance type.
constexpr std::uint64_t kMaxWeight = 0x7FFFFFFFULL;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "lpq_reweight: " << message << "\n";
    std::exit(2);
}

enum class Kind { kUnit, kUniform, kScale, kClip, kLogNormal, kPareto, kBimodal };

struct Transform {
    Kind        kind = Kind::kUnit;
    std::string spec;           // as given; also the seed label
    std::uint64_t i1 = 0;       // integer parameters
    std::uint64_t i2 = 0;
    double        d1 = 0.0;     // real parameters
    double        d2 = 0.0;
    double        d3 = 0.0;
};

[[nodiscard]] std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string              cur;
    for (const char c : s) {
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

[[nodiscard]] std::uint64_t parse_uint(const std::string& raw, const char* what) {
    if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos) {
        fail(std::string(what) + ": '" + raw + "' is not a non-negative integer");
    }
    try {
        return std::stoull(raw);
    } catch (const std::exception&) {
        fail(std::string(what) + ": '" + raw + "' is out of range");
    }
}

[[nodiscard]] double parse_real(const std::string& raw, const char* what) {
    try {
        std::size_t consumed = 0;
        const double v       = std::stod(raw, &consumed);
        if (consumed != raw.size() || !std::isfinite(v)) {
            fail(std::string(what) + ": '" + raw + "' is not a finite number");
        }
        return v;
    } catch (const std::exception&) {
        fail(std::string(what) + ": '" + raw + "' is not a finite number");
    }
}

void expect_arity(const std::vector<std::string>& p, std::size_t n, const std::string& spec) {
    if (p.size() != n) {
        fail("transform '" + spec + "' expects " + std::to_string(n - 1) + " parameter(s)");
    }
}

[[nodiscard]] Transform parse_transform(const std::string& spec) {
    Transform t;
    t.spec                         = spec;
    const std::vector<std::string> p = split(spec, ':');
    const std::string&             name = p[0];

    if (name == "unit") {
        expect_arity(p, 1, spec);
        t.kind = Kind::kUnit;
    } else if (name == "uniform") {
        expect_arity(p, 3, spec);
        t.kind = Kind::kUniform;
        t.i1   = parse_uint(p[1], "uniform LO");
        t.i2   = parse_uint(p[2], "uniform HI");
        if (t.i1 < 1 || t.i1 > t.i2 || t.i2 > kMaxWeight) {
            fail("uniform requires 1 <= LO <= HI <= " + std::to_string(kMaxWeight));
        }
    } else if (name == "scale") {
        expect_arity(p, 2, spec);
        t.kind = Kind::kScale;
        t.i1   = parse_uint(p[1], "scale K");
        if (t.i1 < 1 || t.i1 > kMaxWeight) {
            fail("scale requires 1 <= K <= " + std::to_string(kMaxWeight));
        }
    } else if (name == "clip") {
        expect_arity(p, 2, spec);
        t.kind = Kind::kClip;
        t.i1   = parse_uint(p[1], "clip M");
        if (t.i1 < 1 || t.i1 > kMaxWeight) {
            fail("clip requires 1 <= M <= " + std::to_string(kMaxWeight));
        }
    } else if (name == "lognormal") {
        expect_arity(p, 3, spec);
        t.kind = Kind::kLogNormal;
        t.d1   = parse_real(p[1], "lognormal MEDIAN");
        t.d2   = parse_real(p[2], "lognormal SIGMA");
        if (!(t.d1 > 0.0) || !(t.d2 >= 0.0)) {
            fail("lognormal requires MEDIAN > 0 and SIGMA >= 0");
        }
    } else if (name == "pareto") {
        expect_arity(p, 3, spec);
        t.kind = Kind::kPareto;
        t.d1   = parse_real(p[1], "pareto XMIN");
        t.d2   = parse_real(p[2], "pareto ALPHA");
        if (!(t.d1 >= 1.0) || !(t.d2 > 0.0)) {
            fail("pareto requires XMIN >= 1 and ALPHA > 0");
        }
    } else if (name == "bimodal") {
        expect_arity(p, 4, spec);
        t.kind = Kind::kBimodal;
        t.i1   = parse_uint(p[1], "bimodal SMALL");
        t.i2   = parse_uint(p[2], "bimodal LARGE");
        t.d3   = parse_real(p[3], "bimodal P_LARGE");
        if (t.i1 < 1 || t.i2 < 1 || t.i1 > kMaxWeight || t.i2 > kMaxWeight) {
            fail("bimodal requires SMALL and LARGE in [1, " + std::to_string(kMaxWeight) + "]");
        }
        if (!(t.d3 >= 0.0 && t.d3 <= 1.0)) {
            fail("bimodal requires 0 <= P_LARGE <= 1");
        }
    } else {
        fail("unknown transform '" + name +
             "' (expected unit, uniform, scale, clip, lognormal, pareto or bimodal)");
    }
    return t;
}

/// Counts of DRAWS (a symmetric arc pair is one draw) that landed outside
/// [1, kMaxWeight] and were clamped.
struct ClampCounts {
    std::uint64_t low  = 0;
    std::uint64_t high = 0;
};

/// Rounds a real draw to an integer weight in [1, kMaxWeight], counting clamps.
[[nodiscard]] std::uint64_t finish_real(double v, ClampCounts& c) {
    if (!(v >= 0.0)) {  // negative or NaN cannot arise from the formulas; be loud anyway
        ++c.low;
        return 1;
    }
    const double r = std::floor(v + 0.5);
    if (r < 1.0) {
        ++c.low;
        return 1;
    }
    if (!(r < static_cast<double>(kMaxWeight))) {
        ++c.high;
        return kMaxWeight;
    }
    return static_cast<std::uint64_t>(r);
}

[[nodiscard]] std::uint64_t finish_int(std::uint64_t v, ClampCounts& c) {
    if (v < 1) {
        ++c.low;
        return 1;
    }
    if (v > kMaxWeight) {
        ++c.high;
        return kMaxWeight;
    }
    return v;
}

/// One new weight for an arc whose current weight is `w`.
[[nodiscard]] Weight draw_weight(const Transform& t, Rng& rng, Weight w, ClampCounts& c) {
    std::uint64_t out = 1;
    switch (t.kind) {
        case Kind::kUnit:
            out = 1;
            break;
        case Kind::kUniform:
            out = t.i1 + rng.next_below(t.i2 - t.i1 + 1U);
            break;
        case Kind::kScale:
            out = finish_int(static_cast<std::uint64_t>(w) * t.i1, c);
            break;
        case Kind::kClip:
            out = finish_int(std::min<std::uint64_t>(w, t.i1), c);
            break;
        case Kind::kLogNormal:
            out = finish_real(t.d1 * std::exp(t.d2 * rng.next_normal()), c);
            break;
        case Kind::kPareto: {
            // 1 - U lies in (0, 1], so the power is finite and >= 1.
            const double u = rng.next_unit();
            out            = finish_real(t.d1 * std::pow(1.0 - u, -1.0 / t.d2), c);
            break;
        }
        case Kind::kBimodal:
            out = rng.next_unit() < t.d3 ? t.i2 : t.i1;
            break;
    }
    return static_cast<Weight>(out);
}

struct Reweighted {
    CsrGraph<Weight> graph;
    ClampCounts      clamped;
};

[[nodiscard]] Reweighted reweight(const LoadedGraph<Weight>& loaded, const Transform& t,
                                  std::uint64_t seed) {
    const CsrGraph<Weight>& g = loaded.graph;
    const std::size_t       m = g.num_arcs();

    Rng rng(derive_seed(seed, "lpq_reweight/" + t.spec));

    std::vector<Weight> neu(m, 0);
    std::vector<bool>   done(m, false);
    ClampCounts         clamped;

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (EdgeId e = g.arc_begin(u); e < g.arc_end(u); ++e) {
            if (done[e]) {
                continue;
            }
            const Arc<Weight>& a = g.arc(e);
            neu[e]               = draw_weight(t, rng, a.weight, clamped);
            done[e]              = true;
            if (a.target == u) {
                continue;  // self-loop: its own partner
            }
            // Pair with the first unmatched reverse arc v->u. Adjacency lists
            // are sorted by target, so binary-search to the run of arcs to u.
            const NodeId v     = a.target;
            const EdgeId first = g.arc_begin(v);
            const EdgeId last  = g.arc_end(v);
            EdgeId       lo    = first;
            EdgeId       hi    = last;
            while (lo < hi) {
                const EdgeId mid = lo + (hi - lo) / 2U;
                if (g.arc(mid).target < u) {
                    lo = mid + 1U;
                } else {
                    hi = mid;
                }
            }
            for (EdgeId r = lo; r < last && g.arc(r).target == u; ++r) {
                if (!done[r]) {
                    neu[r]  = neu[e];
                    done[r] = true;
                    break;
                }
            }
        }
    }

    std::vector<InputEdge<Weight>> edges;
    edges.reserve(m);
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (EdgeId e = g.arc_begin(u); e < g.arc_end(u); ++e) {
            edges.push_back(InputEdge<Weight>{u, g.arc(e).target, neu[e]});
        }
    }

    GraphBuildOptions opts;
    opts.directedness        = g.directedness();
    opts.arcs_already_paired = true;  // both directions are already present
    opts.parallel_edges      = ParallelEdgePolicy::kKeep;
    opts.self_loops          = SelfLoopPolicy::kKeep;
    opts.coord_system        = g.coordinate_system();
    opts.dataset_id          = g.dataset_id();

    Reweighted out;
    out.graph   = GraphBuilder<Weight>::build(g.num_nodes(), std::move(edges), opts,
                                              loaded.coordinates);
    out.clamped = clamped;
    return out;
}

void write_stats(const std::string& path, const GraphMetadata& m, std::uint64_t checksum,
                 const ClampCounts& clamped, bool coordinates, bool mapping, bool directed) {
    std::ofstream os(path, std::ios::trunc);
    if (!os) {
        fail("cannot open '" + path + "' for writing");
    }
    JsonWriter w(os);
    w.begin_object();
    w.key_value("graph_checksum_fnv1a64", checksum);
    w.key_value("weight_type", "uint32");
    w.key_value("node_count", static_cast<std::uint64_t>(m.node_count));
    w.key_value("edge_count", static_cast<std::uint64_t>(m.edge_count));
    w.key_value("arc_count", static_cast<std::uint64_t>(m.arc_count));
    w.key_value("directed", directed);
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
    w.key_value("has_coordinates", coordinates);
    w.key_value("has_node_id_mapping", mapping);
    w.key_value("clamped_low_draws", clamped.low);
    w.key_value("clamped_high_draws", clamped.high);
    w.end_object();
    os << "\n";
    if (!os) {
        fail("failed while writing '" + path + "'");
    }
}

void print_usage() {
    std::cout <<
        "Usage: lpq_reweight --input IN.lpqgraph --output OUT.lpqgraph\n"
        "                    --transform SPEC --seed N [--stats-json PATH]\n"
        "SPEC: unit | uniform:LO:HI | scale:K | clip:M | lognormal:MEDIAN:SIGMA |\n"
        "      pareto:XMIN:ALPHA | bimodal:SMALL:LARGE:P_LARGE\n"
        "Refuses to overwrite an existing output. Output weights are integers in\n"
        "[1, 2147483647]. The result is a synthetic-weight graph, not a real network.\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string   input, output, spec, stats;
        std::uint64_t seed      = 0;
        bool          seed_seen = false;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto value = [&](std::string_view flag) -> std::string {
                if (i + 1 >= argc) {
                    fail(std::string(flag) + " requires a value");
                }
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                print_usage();
                return 0;
            } else if (arg == "--input") {
                input = value(arg);
            } else if (arg == "--output") {
                output = value(arg);
            } else if (arg == "--transform") {
                spec = value(arg);
            } else if (arg == "--stats-json") {
                stats = value(arg);
            } else if (arg == "--seed") {
                seed      = parse_uint(value(arg), "--seed");
                seed_seen = true;
            } else {
                fail("unrecognised argument '" + std::string(arg) + "'");
            }
        }
        if (input.empty() || output.empty() || spec.empty() || !seed_seen) {
            fail("--input, --output, --transform and --seed are all required");
        }
        const Transform t = parse_transform(spec);

        namespace fs = std::filesystem;
        if (fs::exists(output)) {
            fail("refusing to overwrite existing output '" + output + "'");
        }
        if (!stats.empty() && fs::exists(stats)) {
            fail("refusing to overwrite existing stats file '" + stats + "'");
        }

        const LoadedGraph<Weight> loaded = read_binary_graph_file<Weight>(input);
        const Reweighted          result = reweight(loaded, t, seed);
        const CsrGraph<Weight>&   g      = result.graph;
        const std::uint64_t       checksum = graph_checksum(g);

        if (fs::path(output).has_parent_path()) {
            fs::create_directories(fs::path(output).parent_path());
        }
        // Write beside the target and rename, so an interrupted run never
        // leaves a truncated file at a path that other tooling will trust.
        const std::string partial = output + ".partial";
        try {
            write_binary_graph_file<Weight>(partial, g, loaded.mapping, loaded.coordinates,
                                            g.coordinate_system());
            if (fs::exists(output)) {
                fs::remove(partial);
                fail("refusing to overwrite existing output '" + output + "'");
            }
            fs::rename(partial, output);
        } catch (...) {
            std::error_code ec;
            fs::remove(partial, ec);
            throw;
        }

        const GraphMetadata m = compute_metadata(g);
        if (!stats.empty()) {
            write_stats(stats, m, checksum, result.clamped, !loaded.coordinates.empty(),
                        !loaded.mapping.empty(), g.directedness() == Directedness::kDirected);
        }

        std::ostringstream line;
        line.precision(12);
        line << "lpq_reweight: n=" << m.node_count << " m=" << m.arc_count
             << " min=" << m.min_weight << " max=" << m.max_weight
             << " mean=" << m.mean_weight << " clamped_low=" << result.clamped.low
             << " clamped_high=" << result.clamped.high << " checksum=" << checksum
             << " checksum_hex=0x" << std::hex << checksum;
        std::cout << line.str() << "\n";
        return 0;
    } catch (const LpqError& e) {
        std::cerr << "lpq_reweight: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "lpq_reweight: " << e.what() << "\n";
        return 1;
    }
}
