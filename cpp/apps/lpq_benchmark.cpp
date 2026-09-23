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
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/graph/query_manifest.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/metrics/experiment_manifest.hpp"
#include "lpq/metrics/hardware_info.hpp"
#include "lpq/metrics/json_writer.hpp"
#include "lpq/metrics/result_record.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
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

    /// A converted dataset (datasets/processed/<id>.lpqgraph) to load instead
    /// of generating a graph. Mutually exclusive with --graph.
    std::string dataset;

    /// A query manifest to run instead of generating queries from a seed.
    std::string queries_file;

    /// An experiment manifest (ESD.md section 22). When set, the runner
    /// measures every combination of algorithm, queue and mode the manifest
    /// lists, in this one process -- see the note in experiment_manifest.hpp on
    /// why one process matters.
    std::string experiment_file;

    std::string mode        = "exact";

    /// Approximation slack for --mode approximate. A returned path may cost up
    /// to (1 + epsilon) times the optimum; the runner checks every query
    /// against that bound rather than trusting the queue to honour it.
    double      epsilon     = 0.0;

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
    bool        graph_spec_given = false;

    /// The seed the QUERIES actually came from. Equal to `seed` when they are
    /// generated here, and the manifest's seed when they are loaded -- which
    /// need not be the same number, and reporting the wrong one would
    /// misattribute the query set.
    SeedType    query_seed = 42;

    /// Candidate-window bounds for the learned queue. The window is the number
    /// of entries the verification layer is allowed to examine per extraction;
    /// it is the knob that trades verification cost against the chance of
    /// skipping a scan (see learned_priority_queue.hpp).
    std::size_t lpq_window_min = 2;
    std::size_t lpq_window_max = 32;

    /// Bucket width for the learned queue's substrate. 0 means "derive it from
    /// the graph" -- see make_queue().
    std::uint64_t lpq_bucket_width = 0;
};

void print_usage() {
    std::cout << R"(lpq_benchmark -- shortest-path benchmark runner

  --algorithm <name>     dijkstra (default) | astar
  --heuristic <name>     zero | euclidean (default) | manhattan | haversine
                         (astar only; euclidean/manhattan/haversine need coordinates)
  --pq <name>            binary_heap | four_ary_heap | pairing_heap |
                         fibonacci_heap | radix_heap | lpq | all (default)
  --lpq-window <lo:hi>   learned-queue candidate window bounds (default 2:32)
  --lpq-bucket-width <n> substrate bucket width (default: derived from weights)
  --graph <spec>         grid:RxC | path:N | star:N | random:N:P  (default grid:200x200)
  --dataset <path>       converted graph (.lpqgraph) to load instead of --graph
  --queries-file <path>  query manifest to run instead of generating queries
  --experiment <path>    experiment manifest; runs the whole algorithm x queue
                         x mode matrix in one pinned process (ESD.md s22)
  --mode <name>          exact (default) | approximate
  --epsilon <e>          approximation slack for --mode approximate; a path may
                         cost up to (1+e) x optimal. Required, and > 0, in that
                         mode. Only the learned queue implements it.
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
            o.graph_spec_given = true;
        } else if (a == "--dataset") {
            o.dataset = next_arg(argc, argv, i, "--dataset");
        } else if (a == "--queries-file") {
            o.queries_file = next_arg(argc, argv, i, "--queries-file");
        } else if (a == "--experiment") {
            o.experiment_file = next_arg(argc, argv, i, "--experiment");
        } else if (a == "--epsilon") {
            o.epsilon = std::stod(next_arg(argc, argv, i, "--epsilon"));
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
        } else if (a == "--lpq-window") {
            const std::string spec  = next_arg(argc, argv, i, "--lpq-window");
            const std::size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                throw InvalidConfigError("--lpq-window expects lo:hi, got '" + spec + "'");
            }
            o.lpq_window_min = std::stoull(spec.substr(0, colon));
            o.lpq_window_max = std::stoull(spec.substr(colon + 1));
            if (o.lpq_window_min == 0 || o.lpq_window_max < o.lpq_window_min) {
                throw InvalidConfigError("--lpq-window bounds must satisfy 0 < lo <= hi");
            }
        } else if (a == "--lpq-bucket-width") {
            o.lpq_bucket_width = std::stoull(next_arg(argc, argv, i, "--lpq-bucket-width"));
            if (o.lpq_bucket_width == 0) {
                throw InvalidConfigError("--lpq-bucket-width must be positive");
            }
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
    if (o.mode != "exact" && o.mode != "approximate") {
        throw InvalidConfigError("unknown --mode: " + o.mode);
    }

    // The two directions of this check exist for the same reason: a run whose
    // label and whose behaviour disagree produces results nobody can interpret
    // afterwards. Approximate mode without slack is exact mode under a weaker
    // label, and exact mode with slack is the opposite and worse -- results
    // reported as exact that were allowed to be wrong.
    if (o.mode == "approximate" && !(o.epsilon > 0.0)) {
        throw InvalidConfigError(
            "--mode approximate requires --epsilon greater than zero; with no slack it is "
            "exact mode reported under a weaker guarantee than it actually provides");
    }
    if (o.mode == "exact" && o.epsilon != 0.0) {
        throw InvalidConfigError(
            "--epsilon is meaningless in exact mode and would be silently ignored, leaving "
            "the command line claiming an approximation that was never applied");
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

// -----------------------------------------------------------------------------
// Queue construction
// -----------------------------------------------------------------------------

/// The learned queue under test, with the control scorer.
///
/// ConstantNodeScorer gives every node the same score, so this configuration
/// measures the STRUCTURE -- bucket substrate, candidate window, verification
/// and correction -- with the model held out. That separation matters: any
/// difference from the classical queues here is attributable to the machinery
/// rather than to prediction quality, which is the only way to tell later
/// whether a model earned its overhead.
using BenchLpq = LearnedPriorityQueue<W, ConstantNodeScorer>;

/// Builds the queue for a measurement.
///
/// Classical queues take no parameters, so the default does. The learned queue
/// needs a bucket width and a window, and both are derived from the graph
/// rather than defaulted, for the same reason the A* heuristic scale is: a
/// bucket width of 1 on a road network whose weights run to six figures puts
/// almost every entry in the overflow ring, which measures the overflow path
/// rather than the queue.
template <typename Queue>
[[nodiscard]] Queue make_queue(const GraphMetadata& meta, const Options& o) {
    if constexpr (std::is_same_v<Queue, BenchLpq>) {
        CandidateWindowConfig window;
        window.k_min = static_cast<std::uint32_t>(o.lpq_window_min);
        window.k_max = static_cast<std::uint32_t>(o.lpq_window_max);

        // Aim for the bucket array to span roughly the range of edge weights,
        // so a bucket holds entries of genuinely similar key rather than
        // everything. Derived from the maximum weight because that is what
        // determines how far a key can jump in one relaxation.
        std::uint64_t width = o.lpq_bucket_width;
        if (width == 0) {
            const auto span = static_cast<std::uint64_t>(
                meta.max_weight > 0.0 ? meta.max_weight : 1.0);
            constexpr auto kBuckets =
                static_cast<std::uint64_t>(BenchLpq::Substrate::kBucketCount);
            width = (span + kBuckets - 1U) / kBuckets;
            if (width == 0) {
                width = 1;
            }
        }
        // The correctness policy is the difference between exact and
        // approximate mode, and it is the ONLY difference: the substrate, the
        // window and the scoring are identical, so a paired exact/approximate
        // comparison isolates the effect of the slack rather than of two
        // separately configured queues.
        const CorrectnessPolicy policy = (o.mode == "approximate")
                                             ? CorrectnessPolicy::approximate(o.epsilon)
                                             : CorrectnessPolicy::exact();
        return BenchLpq{static_cast<Dist>(width), window, policy};
    } else {
        (void)meta;
        (void)o;
        return Queue{};
    }
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
    Queue queue = make_queue<Queue>(meta, o);
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

            // In exact mode any disagreement is a bug. In approximate mode a
            // disagreement is the point, and what must be checked instead is
            // that the guarantee held: the entire value of a bounded-approximate
            // result is its bound, so a run that never verified the bound has
            // measured the speed of an unspecified algorithm.
            //
            // The bound is checked here against the trusted reference rather
            // than taken from the queue's own report. A queue that miscomputes
            // its own slack would otherwise certify itself.
            if (o.mode == "approximate") {
                const double ratio = *rec.approximation_ratio;
                if (ratio > 1.0 + o.epsilon) {
                    std::cerr << "FATAL: " << rec.algorithm << " / " << pq_name
                              << " violated its approximation guarantee on query " << q.source
                              << "->" << q.target << ": ratio " << ratio << " exceeds 1 + "
                              << o.epsilon << "\n";
                    std::exit(2);
                }
            } else if (!*rec.exact_match) {
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

    /// Why this configuration produced no timings, when it produced none.
    ///
    /// Held on the summary as well as on each record because the console is
    /// where a person actually looks. A configuration that silently contributes
    /// no row leaves an empty table and no explanation, which is the failure
    /// this field exists to prevent -- the same reason the records carry it.
    std::string         unsupported_reason;
};

void print_console_summary(const std::map<std::string, QueueSummary>& summaries,
                           const GraphMetadata& meta, const Options& o) {
    std::cout << "\ngraph        : " << meta.dataset_id << "  (" << meta.node_count
              << " nodes, " << meta.edge_count << " edges)\n";
    std::cout << "queries      : " << o.query_count << "  seed " << o.query_seed << "  "
              << (o.single_target ? "single-target" : "full SSSP");
    if (!o.queries_file.empty()) {
        std::cout << "  from " << o.queries_file;
    }
    std::cout << "\n";
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

    // Configurations that produced no timings are listed with their reason
    // rather than left out. An absent row reads as an oversight; an explained
    // one reads as a decision, and only one of those is true.
    for (const auto& [name, s] : summaries) {
        if (s.per_query_median_ns.empty()) {
            std::printf("%-16s %13s   %s\n", name.c_str(), "not measured",
                        s.unsupported_reason.empty() ? "no timings were produced"
                                                     : s.unsupported_reason.c_str());
        }
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
        Options o = parse_args(argc, argv);

        // --- an experiment manifest supplies the run parameters ---------------
        //
        // Applied before anything is loaded, so the rest of main() cannot tell
        // whether its options came from the command line or from a manifest --
        // which is what keeps the batch path from becoming a second, subtly
        // different measurement path.
        //
        // The manifest's values overwrite the defaults rather than merging with
        // flags, and a flag that would contradict the manifest is refused
        // below: a run that reports the manifest's name must be the run the
        // manifest describes.
        ExperimentManifest experiment;
        const bool         batch = !o.experiment_file.empty();
        if (batch) {
            experiment = read_experiment_manifest_file(o.experiment_file);

            if (!o.dataset.empty() || o.graph_spec_given || !o.queries_file.empty()) {
                throw InvalidConfigError(
                    "--experiment supplies the dataset and query set; --dataset, --graph and "
                    "--queries-file would contradict it and are refused rather than silently "
                    "overridden");
            }

            o.dataset =
                "datasets/processed/" + experiment.dataset + ".lpqgraph";
            o.queries_file = experiment.queries_file.empty()
                                 ? ("experiments/manifests/" + experiment.dataset + "_queries_" +
                                    std::to_string(experiment.query_count) + ".yaml")
                                 : experiment.queries_file;
            o.repetitions   = static_cast<std::size_t>(experiment.repetitions);
            o.warmup        = static_cast<std::size_t>(experiment.warmup);
            o.seed          = experiment.seed;
            o.heuristic     = experiment.heuristic;
            o.experiment_id = experiment.experiment;
            if (!experiment.epsilon.empty()) {
                o.epsilon = std::stod(experiment.epsilon);
            }
            // Zero means the manifest did not pin the geometry, so the runner
            // defaults stand. Written this way rather than as an unconditional
            // assignment so that an unset field cannot silently set a window of
            // zero, which would disable the candidate window entirely.
            if (experiment.lpq_window_max != 0) {
                o.lpq_window_min = static_cast<std::size_t>(experiment.lpq_window_min);
                o.lpq_window_max = static_cast<std::size_t>(experiment.lpq_window_max);
            }
            if (experiment.lpq_bucket_width != 0) {
                o.lpq_bucket_width = experiment.lpq_bucket_width;
            }

            // A batch run is the kind whose numbers get published, so it is not
            // allowed to be unpinned. On a hybrid-core laptop an unpinned matrix
            // measures the scheduler as much as the queues.
            o.require_pinning = true;
        }

        o.query_seed  = o.seed;

        if (!o.dataset.empty() && o.graph_spec_given) {
            throw InvalidConfigError(
                "--dataset and --graph are mutually exclusive; the run measures one graph");
        }

        Graph       g;
        std::string graph_label = o.graph_spec;
        if (!o.dataset.empty()) {
            LoadedGraph<W> loaded = read_binary_graph_file<W>(o.dataset);
            g                     = std::move(loaded.graph);
            graph_label           = std::filesystem::path(o.dataset).stem().string();
        } else {
            g = build_graph(o.graph_spec, o.seed);
        }

        GraphMetadata meta = compute_metadata(g);
        if (meta.dataset_id.empty()) {
            meta.dataset_id = graph_label;
        }
        if (g.num_nodes() == 0) {
            throw InvalidConfigError("graph has no nodes");
        }

        std::vector<Query> queries;
        if (!o.queries_file.empty()) {
            const QueryManifest manifest = read_query_manifest_file(o.queries_file);
            // Refuses a manifest generated against a different graph. Without
            // this the run would succeed and silently measure a different set
            // of queries than the one it names.
            manifest.validate_against(g);

            queries.reserve(manifest.queries.size());
            for (const ManifestQuery& q : manifest.queries) {
                queries.push_back(Query{q.source, o.single_target ? q.target : kInvalidNode});
            }
            // An experiment manifest declares how many queries the experiment
            // measures, and the query manifest it resolves to decides how many
            // actually run. If those disagree, the results file names an
            // experiment that was not the one performed -- and because the
            // count below is taken from the queries rather than the
            // declaration, nothing later in the run could notice.
            //
            // This is reachable: the query-manifest path is derived from the
            // dataset id and the declared count, and the naming of those files
            // is not enforced anywhere.
            if (batch && experiment.query_count != queries.size()) {
                throw InvalidConfigError(
                    "experiment '" + experiment.experiment + "' declares " +
                    std::to_string(experiment.query_count) + " queries but " + o.queries_file +
                    " holds " + std::to_string(queries.size()) +
                    "; the results would carry the experiment name and a different query set");
            }

            // Taken from the manifest rather than left at the --queries
            // default, so a run cannot report having measured 20 queries while
            // having actually run the manifest's 1000.
            o.query_count = queries.size();
            o.query_seed  = static_cast<SeedType>(manifest.seed);
        } else {
            queries = generate_queries(g, o.query_count, o.seed, o.single_target);
        }
        const auto hw      = collect_hardware_info();

        // The heuristic scale converts coordinate units into weight units and
        // must not exceed the smallest such ratio anywhere in the graph, or the
        // estimate overshoots the true cost and A* silently returns non-optimal
        // paths. It is therefore derived from the graph rather than defaulted.
        double heuristic_scale = 1.0;
        // In batch mode the matrix decides whether A* runs at all, and o.algorithm
        // still holds its default at this point. Asking the manifest instead of
        // o.algorithm is what stops a matrix containing astar from reaching the
        // search with an underived scale of 1.0 -- which would not fail, it
        // would silently return non-optimal paths.
        const bool needs_heuristic_scale =
            (batch ? std::find(experiment.algorithms.begin(), experiment.algorithms.end(),
                               "astar") != experiment.algorithms.end()
                   : o.algorithm == "astar");

        if (needs_heuristic_scale && o.heuristic != "zero") {
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

        // --- the matrix to measure -------------------------------------------
        //
        // One list for both paths. A command-line run is simply a matrix with
        // one algorithm and one mode, which keeps a single loop below rather
        // than a batch branch that could drift from the interactive one.
        std::vector<ExperimentRun> runs;
        if (batch) {
            runs = experiment.runs();
        } else {
            std::vector<std::string> queue_names;
            if (o.pq == "all") {
                queue_names = {"binary_heap", "four_ary_heap", "pairing_heap", "fibonacci_heap",
                               "radix_heap", "lpq"};
            } else {
                queue_names = {o.pq};
            }
            for (const std::string& name : queue_names) {
                runs.push_back(ExperimentRun{o.algorithm, name, o.mode});
            }
        }

        // Summaries are keyed by queue alone unless the matrix varies the
        // algorithm too, in which case two rows would collide under one name
        // and the second would overwrite the first.
        const bool multi_algorithm =
            std::any_of(runs.begin(), runs.end(), [&runs](const ExperimentRun& r) {
                return r.algorithm != runs.front().algorithm;
            });
        const auto label_of = [multi_algorithm](const ExperimentRun& r) {
            return multi_algorithm ? (r.algorithm + "/" + r.pq) : r.pq;
        };

        if (batch) {
            std::cout << "experiment   : " << experiment.experiment << "\n"
                      << "matrix       : " << runs.size() << " configurations ("
                      << experiment.algorithms.size() << " algorithm x "
                      << experiment.pqs.size() << " queue x " << experiment.modes.size()
                      << " mode)\n";
            if (experiment.collect.hardware_counters) {
                // Requested is not the same as collected. Said here, before the
                // run, so nobody reads the manifest afterwards and assumes the
                // counters in it were gathered.
                std::cout << "hw counters  : requested by the manifest; reported as not "
                             "measured where the host exposes no PMU\n";
            }
        }

        std::vector<ResultRecord>           records;
        std::map<std::string, QueueSummary> summaries;

        for (const auto& run : runs) {
            const std::string name  = run.pq;
            const std::string label = label_of(run);

            // The per-run configuration the measurement actually reads. Set
            // here rather than passed separately so that a matrix entry and an
            // equivalent command line produce identical records.
            o.algorithm = run.algorithm;
            o.mode      = run.mode;

            QueueSummary qs;
            qs.name = label;

            // Only the learned queue has an approximation mechanism. A classical
            // heap asked for approximate mode would run exactly, return exact
            // answers, and emit records labelled "approximate" -- and those
            // records would then sit in a results table next to genuinely
            // approximate ones, faster than them, inviting the conclusion that
            // approximation did not help. The row is emitted with its reason
            // instead, so it is explained rather than absent or wrong.
            const bool queue_supports_mode = (run.mode != "approximate") || (name == "lpq");

            for (const auto& q : queries) {
                ResultRecord rec;
                if (!queue_supports_mode) {
                    rec.pq                 = name;
                    rec.algorithm          = run.algorithm;
                    rec.mode               = run.mode;
                    rec.unsupported_reason =
                        name + " has no approximation mechanism; running it under --mode "
                               "approximate would produce exact results carrying an "
                               "approximate label";
                } else if (name == "binary_heap") {
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
                } else if (name == "lpq") {
                    if constexpr (bucket_substrate_supports<W>()) {
                        rec = measure_one<BenchLpq>(g, meta, q, o, name, hw, heuristic_scale);
                    } else {
                        rec.pq                 = name;
                        rec.unsupported_reason = bucket_substrate_unsupported_reason<W>();
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
                } else if (qs.unsupported_reason.empty()) {
                    // Every query in this configuration carries the same
                    // reason, so the first is taken and the rest would only
                    // repeat it.
                    qs.unsupported_reason = rec.unsupported_reason;
                }
                records.push_back(std::move(rec));
            }
            summaries[label] = std::move(qs);
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
