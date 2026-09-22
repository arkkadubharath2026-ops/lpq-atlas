// =============================================================================
// lpq_benchmark -- the unified benchmark runner (ESD.md section 21).
//
// Usage:
//   lpq_benchmark --algorithm dijkstra --pq binary_heap --graph grid:500x500
//                 --queries 100 --seed 42 --repetitions 11
//                 --output results/raw/run.json
//
// DESIGN NOTES
//
//   * Every queue is measured through the same harness, in the same process,
//     on the same pinned core, over the same query set. PRD.md section 18
//     requires exactly that; running each queue in a separate invocation would
//     let machine state drift between them.
//
//   * Queries are generated deterministically from a seed and are identical
//     across queues, which makes the comparisons paired. Paired comparison is
//     far more powerful than unpaired here, because per-query difficulty varies
//     by orders of magnitude and would otherwise dominate the variance.
//
//   * Correctness is validated against the trusted reference for every query
//     before any timing is reported. A fast wrong answer is not a result.
//
//   * Unsupported configurations still emit a record carrying the reason, so a
//     missing row in a results table is explained rather than absent.
//
// Dataset file loading arrives with the dataset pipeline (ESD.md section 26);
// until then graphs come from the built-in generators, which is enough to
// exercise the whole measurement path end to end.
// =============================================================================
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "lpq/algorithms/astar.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/heuristics.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/metrics/hardware_info.hpp"
#include "lpq/metrics/json_writer.hpp"
#include "lpq/metrics/result_record.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/radix_heap.hpp"

namespace {

using namespace lpq;

using W     = std::uint32_t;
using Dist  = DistanceType<W>;
using Graph = CsrGraph<W>;

// -----------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------

struct Options {
    std::string algorithm   = "dijkstra";
    std::string heuristic   = "euclidean";
    std::string pq          = "all";
    std::string graph_spec  = "grid:200x200";
    std::string mode        = "exact";
    std::string output;
    std::string experiment_id = "adhoc";

    std::size_t query_count = 20;
    std::size_t repetitions = 11;
    std::size_t warmup      = 3;
    SeedType    seed        = 42;
    int         cpu_core    = -1;
    bool        require_pinning = false;
    bool        validate    = true;
    bool        single_target = true;
};

void print_usage() {
    std::cout << R"(lpq_benchmark -- shortest-path benchmark runner

  --algorithm <name>     dijkstra (default) | astar
  --heuristic <name>     zero | euclidean (default) | manhattan | haversine
                         (astar only; euclidean/manhattan/haversine need coordinates)
  --pq <name>            binary_heap | four_ary_heap | pairing_heap |
                         fibonacci_heap | radix_heap | all (default)
  --graph <spec>         grid:RxC | path:N | star:N | random:N:P  (default grid:200x200)
  --mode <name>          exact (default)
  --queries <n>          number of source-target pairs (default 20)
  --repetitions <n>      measured repetitions per query (default 11)
  --warmup <n>           discarded warm-up repetitions (default 3)
  --seed <n>             query-generation seed (default 42)
  --cpu-core <n>         core to pin to (default: auto, avoids core 0)
  --require-pinning      fail rather than run unpinned
  --no-validate          skip reference validation (NOT for reported results)
  --sssp                 full single-source instead of single-target
  --experiment-id <s>    label written into every record
  --output <path>        JSON results file (default: stdout summary only)
  --help
)";
}

[[nodiscard]] std::string next_arg(int argc, char** argv, int& i, const char* name) {
    if (i + 1 >= argc) {
        throw InvalidConfigError(std::string(name) + " requires a value");
    }
    return argv[++i];
}

[[nodiscard]] Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_usage();
            std::exit(0);
        } else if (a == "--algorithm") {
            o.algorithm = next_arg(argc, argv, i, "--algorithm");
        } else if (a == "--heuristic") {
            o.heuristic = next_arg(argc, argv, i, "--heuristic");
        } else if (a == "--pq") {
            o.pq = next_arg(argc, argv, i, "--pq");
        } else if (a == "--graph") {
            o.graph_spec = next_arg(argc, argv, i, "--graph");
        } else if (a == "--mode") {
            o.mode = next_arg(argc, argv, i, "--mode");
        } else if (a == "--queries") {
            o.query_count = std::stoull(next_arg(argc, argv, i, "--queries"));
        } else if (a == "--repetitions") {
            o.repetitions = std::stoull(next_arg(argc, argv, i, "--repetitions"));
        } else if (a == "--warmup") {
            o.warmup = std::stoull(next_arg(argc, argv, i, "--warmup"));
        } else if (a == "--seed") {
            o.seed = std::stoull(next_arg(argc, argv, i, "--seed"));
        } else if (a == "--cpu-core") {
            o.cpu_core = std::stoi(next_arg(argc, argv, i, "--cpu-core"));
        } else if (a == "--require-pinning") {
            o.require_pinning = true;
        } else if (a == "--no-validate") {
            o.validate = false;
        } else if (a == "--sssp") {
            o.single_target = false;
        } else if (a == "--experiment-id") {
            o.experiment_id = next_arg(argc, argv, i, "--experiment-id");
        } else if (a == "--output") {
            o.output = next_arg(argc, argv, i, "--output");
        } else {
            throw InvalidConfigError("unknown argument: " + a);
        }
    }
    if (o.repetitions == 0) {
        throw InvalidConfigError("--repetitions must be at least 1");
    }
    if (o.algorithm == "astar" && !o.single_target) {
        // A* is inherently point-to-point; there is no "A* to everywhere".
        throw InvalidConfigError("--sssp is incompatible with --algorithm astar");
    }
    if (o.algorithm != "dijkstra" && o.algorithm != "astar") {
        throw InvalidConfigError("unknown --algorithm: " + o.algorithm);
    }
    return o;
}

// -----------------------------------------------------------------------------
// Graph construction
// -----------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::size_t              start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

[[nodiscard]] Graph build_graph(const std::string& spec, SeedType seed) {
    const auto parts = split(spec, ':');
    if (parts.empty()) {
        throw InvalidConfigError("empty --graph spec");
    }
    const std::string& kind = parts[0];

    if (kind == "grid") {
        if (parts.size() != 2) {
            throw InvalidConfigError("grid spec must be grid:RxC");
        }
        const auto dims = split(parts[1], 'x');
        if (dims.size() != 2) {
            throw InvalidConfigError("grid spec must be grid:RxC");
        }
        const auto rows = std::stoull(dims[0]);
        const auto cols = std::stoull(dims[1]);
        std::vector<InputEdge<W>> edges;
        std::vector<Coordinate>   coords(rows * cols);
        auto id = [cols](std::size_t r, std::size_t c) {
            return static_cast<NodeId>(r * cols + c);
        };
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                coords[id(r, c)] = Coordinate{static_cast<double>(c), static_cast<double>(r)};
                if (c + 1 < cols) {
                    edges.push_back({id(r, c), id(r, c + 1), 1});
                }
                if (r + 1 < rows) {
                    edges.push_back({id(r, c), id(r + 1, c), 1});
                }
            }
        }
        GraphBuildOptions opts;
        opts.directedness = Directedness::kUndirected;
        opts.coord_system = CoordinateSystem::kEuclidean;
        opts.dataset_id   = spec;
        return GraphBuilder<W>::build(rows * cols, std::move(edges), opts, std::move(coords));
    }

    if (kind == "path") {
        const auto n = std::stoull(parts.at(1));
        std::vector<InputEdge<W>> edges;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            edges.push_back({static_cast<NodeId>(i), static_cast<NodeId>(i + 1), 1});
        }
        GraphBuildOptions opts;
        opts.dataset_id = spec;
        return GraphBuilder<W>::build(n, std::move(edges), opts);
    }

    if (kind == "star") {
        const auto n = std::stoull(parts.at(1));
        std::vector<InputEdge<W>> edges;
        for (std::size_t i = 1; i < n; ++i) {
            edges.push_back({0, static_cast<NodeId>(i), 1});
        }
        GraphBuildOptions opts;
        opts.directedness = Directedness::kUndirected;
        opts.dataset_id   = spec;
        return GraphBuilder<W>::build(n, std::move(edges), opts);
    }

    if (kind == "random") {
        // random:N:P -- N nodes, per-ordered-pair edge probability P.
        const auto n = std::stoull(parts.at(1));
        const double p = parts.size() > 2 ? std::stod(parts[2]) : 0.01;
        Rng rng(derive_seed(seed, "graph"));
        std::vector<InputEdge<W>> edges;
        for (std::size_t u = 0; u < n; ++u) {
            for (std::size_t v = 0; v < n; ++v) {
                if (u != v && rng.next_unit() < p) {
                    edges.push_back({static_cast<NodeId>(u), static_cast<NodeId>(v),
                                     static_cast<W>(rng.next_in_range(1, 1000))});
                }
            }
        }
        GraphBuildOptions opts;
        opts.dataset_id = spec;
        return GraphBuilder<W>::build(n, std::move(edges), opts);
    }

    throw InvalidConfigError("unknown graph kind: " + kind);
}

// -----------------------------------------------------------------------------
// Query generation
// -----------------------------------------------------------------------------

struct Query {
    NodeId source;
    NodeId target;
};

/// Deterministic from the manifest seed, so every queue sees identical queries
/// and the comparison is paired (PRD.md section 18).
[[nodiscard]] std::vector<Query> generate_queries(const Graph& g, std::size_t count,
                                                  SeedType seed, bool single_target) {
    Rng                rng(derive_seed(seed, "queries"));
    std::vector<Query> qs;
    qs.reserve(count);
    const auto n = g.num_nodes();
    for (std::size_t i = 0; i < count; ++i) {
        const NodeId s = rng.next_node(n);
        const NodeId t = single_target ? rng.next_node(n) : kInvalidNode;
        qs.push_back({s, t});
    }
    return qs;
}

// -----------------------------------------------------------------------------
// Running one (queue, query) pair
// -----------------------------------------------------------------------------

/// Dispatches an A* run on the heuristic named in the options.
///
/// The heuristic is a template parameter, so each variant is a separately
/// compiled, fully inlined search -- there is no virtual call per node. The
/// dispatch happens once per query, outside the timed region.
template <typename Queue>
ShortestPathResult<W> run_astar(const Graph& g, NodeId source, NodeId target, Queue& queue,
                                const std::string& heuristic, double scale) {
    AstarOptions aopts;
    aopts.target       = target;
    aopts.measure_time = false;

    if (heuristic == "zero") {
        ZeroHeuristic<W> h;
        return astar<W, Queue, ZeroHeuristic<W>>(g, source, queue, h, aopts);
    }
    if (heuristic == "euclidean") {
        EuclideanHeuristic<W> h(scale);
        return astar<W, Queue, EuclideanHeuristic<W>>(g, source, queue, h, aopts);
    }
    if (heuristic == "manhattan") {
        ManhattanHeuristic<W> h(scale);
        return astar<W, Queue, ManhattanHeuristic<W>>(g, source, queue, h, aopts);
    }
    if (heuristic == "haversine") {
        HaversineHeuristic<W> h(scale);
        return astar<W, Queue, HaversineHeuristic<W>>(g, source, queue, h, aopts);
    }
    throw InvalidConfigError("unknown --heuristic: " + heuristic);
}

/// Runs and validates one query on one queue.
template <typename Queue>
ResultRecord measure_one(const Graph& g, const GraphMetadata& meta, const Query& q,
                         const Options& o, const std::string& pq_name,
                         const HardwareInfo& hw, double heuristic_scale) {
    ResultRecord rec;
    rec.experiment_id = o.experiment_id;
    rec.timestamp     = iso8601_now();
    rec.dataset       = g.dataset_id();
    rec.algorithm     = o.algorithm == "astar" ? ("astar:" + o.heuristic) : o.algorithm;
    rec.pq            = pq_name;
    rec.mode          = o.mode;
    rec.graph_nodes   = meta.node_count;
    rec.graph_edges   = meta.edge_count;
    rec.source        = q.source;
    rec.target        = q.target;
    rec.hardware      = hw;
    rec.hw_counters.unavailable_reason =
        hw.hardware_counters_available
            ? ""
            : "no CPU PMU on this host; see docs/experiments/profiling.md. Counters "
              "are reported as null and are never estimated.";

    DijkstraOptions dopts;
    dopts.target       = q.target;
    dopts.measure_time = false;  // the harness owns timing

    // Allocated once, outside the timed region, and reused across repetitions.
    Queue queue;
    queue.reserve_nodes(g.num_nodes());

    const bool is_astar = (o.algorithm == "astar");

    // The exact call that will be timed, so validation and measurement cannot
    // diverge.
    auto run_once = [&]() {
        return is_astar ? run_astar<Queue>(g, q.source, q.target, queue, o.heuristic,
                                           heuristic_scale)
                        : dijkstra<W, Queue>(g, q.source, queue, dopts);
    };

    // --- Correctness first: a fast wrong answer is not a result -------------
    const auto candidate = run_once();
    if (o.validate) {
        ReferenceDijkstraOptions ropts;
        ropts.target = q.target;
        const auto reference = reference_dijkstra_heap(g, q.source, ropts);

        const NodeId probe = (q.target == kInvalidNode) ? q.source : q.target;
        const bool   reachable = candidate.reachable(probe);
        rec.target_reachable   = reachable;

        if (reachable && reference.reachable(probe)) {
            rec.path_cost    = static_cast<double>(candidate.dist[probe]);
            rec.optimal_cost = static_cast<double>(reference.dist[probe]);
            rec.approximation_ratio =
                approximation_ratio<W>(candidate.dist[probe], reference.dist[probe]);
            rec.exact_match = costs_equal<W>(candidate.dist[probe], reference.dist[probe]);

            if (!*rec.exact_match) {
                std::cerr << "FATAL: " << rec.algorithm << " / " << pq_name
                          << " disagreed with the reference on query " << q.source << "->"
                          << q.target << " (" << candidate.dist[probe] << " vs "
                          << reference.dist[probe] << ")\n";
                std::exit(2);
            }
        } else {
            rec.exact_match = (reachable == reference.reachable(probe));
        }
    }
    rec.termination = to_string(candidate.termination);
    rec.counters    = candidate.counters;

    // --- Timing --------------------------------------------------------------
    BenchmarkConfig bcfg;
    bcfg.repetitions        = o.repetitions;
    bcfg.warmup_repetitions = o.warmup;
    bcfg.cpu_core           = o.cpu_core;
    bcfg.require_pinning    = o.require_pinning;

    // The value consumed must be a genuine OUTPUT of the search, not a
    // counter. Counters compile out entirely when instrumentation is disabled
    // (ESD.md section 19), so consuming settled_nodes would hand the optimizer
    // a constant zero in exactly the build configuration used to measure the
    // true hot path -- the one where a deleted search would be least visible.
    // The probed distance is always computed.
    const NodeId probe_node = (q.target == kInvalidNode) ? q.source : q.target;

    rec.timing = run_benchmark(
        [&] {
            const auto r = run_once();
            do_not_optimize(r.dist[probe_node]);
        },
        bcfg);

    return rec;
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------

/// Aggregates one queue's results across the query set.
///
/// Two distinct dispersions are tracked, because conflating them is misleading:
///
///   * WITHIN-query spread is measurement noise -- the same query timed
///     repeatedly on a pinned core. This is what says whether a timing is
///     trustworthy, and a large value means interference.
///
///   * ACROSS-query spread is workload heterogeneity. Different source-target
///     pairs genuinely take different amounts of work -- on a grid, a query
///     across the diagonal settles far more nodes than a neighbouring one --
///     so a large value here is a property of the query set, not a defect.
///
/// An earlier version of this report showed only the across-query figure and
/// labelled it "MAD", which made a perfectly clean measurement look like a
/// 57%-noisy one.
struct QueueSummary {
    std::string         name;
    /// One median per query: the robust per-query timing.
    std::vector<double> per_query_median_ns;
    /// One relative MAD per query: the per-query measurement noise.
    std::vector<double> per_query_relative_mad;
    std::uint64_t       total_comparisons = 0;
    std::uint64_t       total_pops        = 0;
    bool                any_unpinned      = false;
    std::size_t         noisy_queries     = 0;
};

void print_console_summary(const std::map<std::string, QueueSummary>& summaries,
                           const GraphMetadata& meta, const Options& o) {
    std::cout << "\ngraph        : " << meta.dataset_id << "  (" << meta.node_count
              << " nodes, " << meta.edge_count << " edges)\n";
    std::cout << "queries      : " << o.query_count << "  seed " << o.seed << "  "
              << (o.single_target ? "single-target" : "full SSSP") << "\n";
    std::cout << "repetitions  : " << o.repetitions << " measured, " << o.warmup
              << " warm-up\n\n";

    // The headline is the median of the per-query medians, with the across-query
    // MAD beside it. The mean is deliberately not shown: see statistics.hpp.
    std::printf("%-16s %13s %12s %9s %9s %14s %12s\n", "queue", "median ns",
                "q-MAD ns", "q-spread", "noise", "comparisons", "pops");
    std::printf("%-16s %13s %12s %9s %9s %14s %12s\n", "----------------", "-------------",
                "------------", "---------", "---------", "--------------", "------------");

    // Ranked by median so the fastest queue is visible at a glance.
    std::vector<const QueueSummary*> ordered;
    for (const auto& [name, s] : summaries) {
        if (!s.per_query_median_ns.empty()) {
            ordered.push_back(&s);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const QueueSummary* a, const QueueSummary* b) {
        return summarise(a->per_query_median_ns).median <
               summarise(b->per_query_median_ns).median;
    });

    for (const QueueSummary* s : ordered) {
        const auto across = summarise(s->per_query_median_ns);
        const auto within = summarise(s->per_query_relative_mad);
        std::printf("%-16s %13.0f %12.0f %8.1f%% %8.1f%% %14llu %12llu%s%s\n",
                    s->name.c_str(), across.median, across.mad,
                    across.relative_mad() * 100.0,
                    within.median * 100.0,
                    static_cast<unsigned long long>(s->total_comparisons),
                    static_cast<unsigned long long>(s->total_pops),
                    s->any_unpinned ? "  [UNPINNED]" : "",
                    s->noisy_queries > 0 ? "  [NOISY]" : "");
    }

    std::cout << "\n  median ns   median across queries of each query's median timing\n"
                 "  q-MAD ns    MAD across queries -- workload heterogeneity, not noise\n"
                 "  q-spread    the same, relative to the median\n"
                 "  noise       typical WITHIN-query relative MAD -- the measurement noise\n";

    // Speedups are stated against the fastest queue, and only when the
    // measurement is clean enough to support the claim.
    if (ordered.size() > 1) {
        const double best = summarise(ordered.front()->per_query_median_ns).median;
        std::cout << "\nrelative to " << ordered.front()->name << " (fastest):\n";
        for (std::size_t i = 1; i < ordered.size(); ++i) {
            const double m = summarise(ordered[i]->per_query_median_ns).median;
            std::printf("  %-16s %.2fx slower\n", ordered[i]->name.c_str(),
                        best > 0.0 ? m / best : 0.0);
        }
    }

    std::size_t unpinned = 0;
    std::size_t noisy    = 0;
    for (const QueueSummary* s : ordered) {
        unpinned += s->any_unpinned ? 1U : 0U;
        noisy += s->noisy_queries;
    }
    if (unpinned > 0) {
        std::cout << "\nWARNING: some runs were not pinned to a core; those timings carry "
                     "scheduler-migration noise and must not be compared with pinned ones.\n";
    }
    if (noisy > 0) {
        std::cout << "\nNOTE: " << noisy
                  << " query measurement(s) exceeded the within-query noise threshold. "
                     "Every raw sample is retained in the JSON output; none were discarded.\n";
    }

    std::cout << "\nHardware counters: not collected on this host (no CPU PMU). "
                 "Reported as null, never estimated. See docs/experiments/profiling.md.\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options o = parse_args(argc, argv);

        const Graph         g    = build_graph(o.graph_spec, o.seed);
        const GraphMetadata meta = compute_metadata(g);
        if (g.num_nodes() == 0) {
            throw InvalidConfigError("graph has no nodes");
        }

        const auto queries = generate_queries(g, o.query_count, o.seed, o.single_target);
        const auto hw      = collect_hardware_info();

        // The heuristic scale converts coordinate units into weight units and
        // must not exceed the smallest such ratio anywhere in the graph, or the
        // estimate overshoots the true cost and A* silently returns non-optimal
        // paths. It is therefore derived from the graph rather than defaulted.
        double heuristic_scale = 1.0;
        if (o.algorithm == "astar" && o.heuristic != "zero") {
            if (!g.has_coordinates()) {
                throw UnsupportedConfigurationError(
                    "--heuristic " + o.heuristic + " requires a graph with coordinates; "
                    "use --heuristic zero");
            }
            heuristic_scale = euclidean_admissible_scale(g);
            if (!(heuristic_scale > 0.0)) {
                throw UnsupportedConfigurationError(
                    "no admissible heuristic scale could be derived for this graph");
            }
            std::cout << "heuristic    : " << o.heuristic
                      << "  (admissible scale " << heuristic_scale << ")\n";
        }

        if (hw.git_commit == "unknown") {
            std::cerr << "refusing to run: this binary carries no git provenance "
                         "(ESD.md section 40)\n";
            return 3;
        }
        if (hw.git_dirty) {
            std::cerr << "WARNING: built from a dirty working tree; results are not "
                         "reproducible from the recorded commit alone.\n";
        }

        std::vector<std::string> queue_names;
        if (o.pq == "all") {
            queue_names = {"binary_heap", "four_ary_heap", "pairing_heap", "fibonacci_heap",
                           "radix_heap"};
        } else {
            queue_names = {o.pq};
        }

        std::vector<ResultRecord>           records;
        std::map<std::string, QueueSummary> summaries;

        for (const auto& name : queue_names) {
            QueueSummary qs;
            qs.name = name;

            for (const auto& q : queries) {
                ResultRecord rec;
                if (name == "binary_heap") {
                    rec = measure_one<BinaryHeap<Dist>>(g, meta, q, o, name, hw, heuristic_scale);
                } else if (name == "four_ary_heap") {
                    rec = measure_one<FourAryHeap<Dist>>(g, meta, q, o, name, hw, heuristic_scale);
                } else if (name == "pairing_heap") {
                    rec = measure_one<PairingHeap<Dist>>(g, meta, q, o, name, hw, heuristic_scale);
                } else if (name == "fibonacci_heap") {
                    rec = measure_one<FibonacciHeap<Dist>>(g, meta, q, o, name, hw, heuristic_scale);
                } else if (name == "radix_heap") {
                    if constexpr (radix_heap_supports<W>()) {
                        rec = measure_one<RadixHeap<Dist>>(g, meta, q, o, name, hw, heuristic_scale);
                    } else {
                        rec.pq                 = name;
                        rec.unsupported_reason = radix_heap_unsupported_reason<W>();
                    }
                } else {
                    throw InvalidConfigError("unknown --pq: " + name);
                }

                if (rec.supported()) {
                    qs.per_query_median_ns.push_back(rec.timing.median_ns());
                    qs.per_query_relative_mad.push_back(rec.timing.summary.relative_mad());
                    qs.total_comparisons += rec.counters.pq_comparisons;
                    qs.total_pops += rec.counters.pq_pops;
                    qs.any_unpinned = qs.any_unpinned || !rec.timing.affinity.pinned;
                    qs.noisy_queries += rec.timing.noisy ? 1U : 0U;
                }
                records.push_back(std::move(rec));
            }
            summaries[name] = std::move(qs);
        }

        print_console_summary(summaries, meta, o);

        if (!o.output.empty()) {
            const std::filesystem::path path(o.output);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            // Raw results are immutable (ESD.md section 31): refuse to overwrite.
            if (std::filesystem::exists(path)) {
                std::cerr << "refusing to overwrite an existing results file: " << o.output
                          << "\nRaw results are immutable; choose a new path.\n";
                return 4;
            }
            std::ofstream out(path);
            if (!out) {
                throw LpqError("cannot open output file: " + o.output);
            }
            JsonWriter w(out, true);
            w.begin_object();
            w.key_value("schema_version", static_cast<std::int64_t>(kResultSchemaVersion));
            w.key_value("experiment_id", o.experiment_id);
            w.key_value("generated_at", iso8601_now());
            w.begin_array("records");
            for (const auto& r : records) {
                write_record(w, r);
            }
            w.end_array();
            w.end_object();
            out << '\n';
            std::cout << "\nwrote " << records.size() << " records to " << o.output << "\n";
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
