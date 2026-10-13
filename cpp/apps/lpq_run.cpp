// =============================================================================
// lpq_run -- one query, machine-readable (LPQ Atlas spec sections 7-9).
//
// The execution contract the web orchestration layer drives. It runs exactly
// one source -> target query through the same templated Dijkstra/A* and the
// same queue types lpq_benchmark measures, and writes:
//
//   * a versioned JSON result (distance, path, counters, learned-queue
//     statistics, correctness against the trusted reference, timings), and
//   * optionally a JSONL trace of the real execution (trace_recorder.hpp).
//
// Three executions, kept apart on purpose (spec 3.4):
//
//   1. The AUTHORITATIVE run: untraced, its result and counters are reported.
//   2. The TRACED run: a second, instrumented execution whose only product is
//      the event stream. Its result is compared with run 1 and the comparison
//      is reported -- a trace that described a different search than the one
//      whose metrics are shown would be a fabricated trace.
//   3. The TIMED runs: the benchmark harness times run 1's exact call with no
//      tracing. This is the only runtime reported as the algorithm's runtime;
//      the traced run's wall clock is reported separately and labelled.
//
// The trusted reference is reference_dijkstra_heap, the same one lpq_benchmark
// validates every query against.
// =============================================================================
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/adaptation/controller_observer.hpp"
#include "lpq/adaptation/online_model_check.hpp"
#include "lpq/adaptation/online_session.hpp"
#include "lpq/algorithms/astar.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/heuristics.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/error.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/lpq/pooled_bucket_queue.hpp"
#include "lpq/lpq/queue_autotune.hpp"
#include "lpq/lpq/queue_policy.hpp"
#include "lpq/lpq/queue_config.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/metrics/hardware_info.hpp"
#include "lpq/metrics/json_writer.hpp"
#include "lpq/metrics/result_record.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/model_report.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/radix_heap.hpp"
#include "lpq/trace/trace_recorder.hpp"

namespace {

using namespace lpq;

using W     = std::uint32_t;
using Dist  = DistanceType<W>;
using Graph = CsrGraph<W>;

constexpr int kRunSchemaVersion = 1;

using Scorer    = StaticNodeScorer<LogisticPredictor>;
using LpqPlain  = LearnedPriorityQueue<W, ConstantNodeScorer>;
using LpqStatic = LearnedPriorityQueue<W, Scorer>;
using LpqOnline = LearnedPriorityQueue<W, OnlineSession::Scorer>;

// -----------------------------------------------------------------------------
// Errors are categorised so the API can map them to safe user-facing messages
// -----------------------------------------------------------------------------

struct CategorisedError : std::runtime_error {
    CategorisedError(std::string cat, const std::string& msg)
        : std::runtime_error(msg), category(std::move(cat)) {}
    std::string category;
};

[[noreturn]] void invalid(const std::string& msg) { throw CategorisedError("invalid_request", msg); }

// -----------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------

struct Options {
    std::string   dataset;
    std::string   run_id    = "adhoc";
    std::string   algorithm = "dijkstra";
    std::string   heuristic = "auto";
    std::string   pq        = "binary_heap";
    std::string   model;
    bool          adaptive_learning = false;

    // --- lpq_online (M5-O4) ---------------------------------------------------------------
    std::string   online_model;               ///< warm-start artifact (.lpqmodel)
    std::string   online_report;              ///< its .report.json
    std::string   online_start = "warm";      ///< warm | cold
    std::string   online_update_mode = "periodic";
    std::string   online_apply = "between_queries";
    bool          allow_graph_mismatch = false;
    bool          allow_width_mismatch = false;
    bool          online_no_provenance = false;
    std::string   policy;  ///< queue policy file for --pq lpq_optimized (empty: none)
    bool          autotune = true;  ///< lpq_optimized: measure candidates on this graph and machine
    std::string   mode    = "exact";
    double        epsilon = 0.0;
    std::uint64_t source  = 0;
    std::uint64_t target  = 0;
    bool          have_source = false;
    bool          have_target = false;

    std::size_t   window_min   = 2;
    std::size_t   window_max   = 32;
    std::uint64_t bucket_width = 0;

    TraceLevel    trace_level = TraceLevel::kNone;
    std::uint64_t max_events  = 250000;
    std::string   trace_out;
    std::string   result_out;

    std::size_t repetitions = 5;
    std::size_t warmup      = 1;
    int         cpu_core    = -1;
};

void print_usage() {
    std::cout << R"(lpq_run -- run one query and emit machine-readable output

  --dataset <path>        converted graph (.lpqgraph)                 [required]
  --source <n> --target <n>                                           [required]
  --algorithm <name>      dijkstra (default) | astar
  --heuristic <name>      auto (default) | zero | euclidean | manhattan | haversine
  --pq <name>             binary_heap (default) | four_ary_heap | pairing_heap |
                          fibonacci_heap | radix_heap | lpq | lpq_model | lpq_adaptive | lpq_online |
                          lpq_optimized (exact; measures candidate queues on this
                          graph and machine, Dijkstra only, A* uses radix)
  --model <path>          trained model; required by, and only valid with, lpq_model
  --adaptive-learning     with lpq_adaptive: also update the model online
  --online-model <path>   warm-start model artifact for --pq lpq_online (required unless cold)
  --online-model-report <path>
                          the .report.json written with that artifact; its graph checksum and
                          bucket width are checked against this run
  --online-start <s>      warm (default) | cold (zero-initialised; no artifact)
  --online-update-mode <m>  disabled | every_event | periodic (default) | adaptive. Updates
                          are applied BETWEEN queries: the search itself runs on the start model
  --online-apply <p>      between_queries (the only policy implemented)
  --allow-graph-mismatch  accept a model trained on another graph (recorded in the result)
  --allow-width-mismatch  accept a model trained for another bucket width (recorded)
  --online-model-no-provenance
                          accept a model with no report (recorded)
  --mode <name>           exact (default) | approximate (learned queues only)
  --epsilon <e>           slack for approximate mode, 0 < e <= 1
  --lpq-window <lo:hi>    learned-queue candidate window (default 2:32)
  --policy <path>         queue policy for --pq lpq_optimized (overrides autotuning)
  --autotune <on|off>     lpq_optimized: calibrate on this graph and machine (default on;
                          Dijkstra only; A* resolves to the radix heap)
  --lpq-bucket-width <n>  substrate bucket width (default derived from weights)
  --trace-level <l>       none (default) | summary | visual | full
  --max-events <n>        trace event cap (default 250000)
  --trace-out <path>      JSONL trace destination (required unless level none)
  --result-out <path>     JSON result destination (default stdout)
  --repetitions <n>       clean timed repetitions, 0 disables timing (default 5)
  --warmup <n>            discarded warm-up repetitions (default 1)
  --cpu-core <n>          core to pin timed runs to (default auto)
  --run-id <s>            identifier written into the result and trace
)";
}

[[nodiscard]] std::string next_arg(int argc, char** argv, int& i, const char* name) {
    if (i + 1 >= argc) {
        invalid(std::string(name) + " requires a value");
    }
    return argv[++i];
}

[[nodiscard]] std::uint64_t parse_u64(const std::string& s, const char* name) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        invalid(std::string(name) + " expects a non-negative integer, got '" + s + "'");
    }
    try {
        return std::stoull(s);
    } catch (const std::exception&) {
        invalid(std::string(name) + " is out of range: '" + s + "'");
    }
}

[[nodiscard]] Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_usage();
            std::exit(0);
        } else if (a == "--dataset") {
            o.dataset = next_arg(argc, argv, i, "--dataset");
        } else if (a == "--run-id") {
            o.run_id = next_arg(argc, argv, i, "--run-id");
        } else if (a == "--algorithm") {
            o.algorithm = next_arg(argc, argv, i, "--algorithm");
        } else if (a == "--heuristic") {
            o.heuristic = next_arg(argc, argv, i, "--heuristic");
        } else if (a == "--pq") {
            o.pq = next_arg(argc, argv, i, "--pq");
        } else if (a == "--model") {
            o.model = next_arg(argc, argv, i, "--model");
        } else if (a == "--adaptive-learning") {
            o.adaptive_learning = true;
        } else if (a == "--online-model") {
            o.online_model = next_arg(argc, argv, i, "--online-model");
        } else if (a == "--online-model-report") {
            o.online_report = next_arg(argc, argv, i, "--online-model-report");
        } else if (a == "--online-start") {
            o.online_start = next_arg(argc, argv, i, "--online-start");
        } else if (a == "--online-update-mode") {
            o.online_update_mode = next_arg(argc, argv, i, "--online-update-mode");
        } else if (a == "--online-apply") {
            o.online_apply = next_arg(argc, argv, i, "--online-apply");
        } else if (a == "--allow-graph-mismatch") {
            o.allow_graph_mismatch = true;
        } else if (a == "--allow-width-mismatch") {
            o.allow_width_mismatch = true;
        } else if (a == "--online-model-no-provenance") {
            o.online_no_provenance = true;
        } else if (a == "--mode") {
            o.mode = next_arg(argc, argv, i, "--mode");
        } else if (a == "--epsilon") {
            const std::string v = next_arg(argc, argv, i, "--epsilon");
            try {
                std::size_t used = 0;
                o.epsilon        = std::stod(v, &used);
                if (used != v.size()) {
                    invalid("--epsilon is not a number: '" + v + "'");
                }
            } catch (const std::logic_error&) {
                invalid("--epsilon is not a number: '" + v + "'");
            }
        } else if (a == "--source") {
            o.source      = parse_u64(next_arg(argc, argv, i, "--source"), "--source");
            o.have_source = true;
        } else if (a == "--target") {
            o.target      = parse_u64(next_arg(argc, argv, i, "--target"), "--target");
            o.have_target = true;
        } else if (a == "--lpq-window") {
            const std::string spec  = next_arg(argc, argv, i, "--lpq-window");
            const std::size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                invalid("--lpq-window expects lo:hi, got '" + spec + "'");
            }
            o.window_min = parse_u64(spec.substr(0, colon), "--lpq-window");
            o.window_max = parse_u64(spec.substr(colon + 1), "--lpq-window");
            if (o.window_min == 0 || o.window_max < o.window_min || o.window_max > 4096) {
                invalid("--lpq-window bounds must satisfy 0 < lo <= hi <= 4096");
            }
        } else if (a == "--lpq-bucket-width") {
            o.bucket_width = parse_u64(next_arg(argc, argv, i, "--lpq-bucket-width"),
                                       "--lpq-bucket-width");
            if (o.bucket_width == 0) {
                invalid("--lpq-bucket-width must be positive");
            }
        } else if (a == "--trace-level") {
            try {
                o.trace_level = parse_trace_level(next_arg(argc, argv, i, "--trace-level"));
            } catch (const InvalidConfigError& e) {
                invalid(e.what());
            }
        } else if (a == "--max-events") {
            o.max_events = parse_u64(next_arg(argc, argv, i, "--max-events"), "--max-events");
        } else if (a == "--trace-out") {
            o.trace_out = next_arg(argc, argv, i, "--trace-out");
        } else if (a == "--result-out") {
            o.result_out = next_arg(argc, argv, i, "--result-out");
        } else if (a == "--repetitions") {
            o.repetitions = parse_u64(next_arg(argc, argv, i, "--repetitions"), "--repetitions");
        } else if (a == "--warmup") {
            o.warmup = parse_u64(next_arg(argc, argv, i, "--warmup"), "--warmup");
        } else if (a == "--cpu-core") {
            o.cpu_core = static_cast<int>(parse_u64(next_arg(argc, argv, i, "--cpu-core"),
                                                    "--cpu-core"));
        } else if (a == "--policy") {
            o.policy = next_arg(argc, argv, i, "--policy");
        } else if (a == "--autotune") {
            const std::string v = next_arg(argc, argv, i, "--autotune");
            if (v != "on" && v != "off") invalid("--autotune expects on or off");
            o.autotune = (v == "on");
        } else {
            invalid("unknown argument: " + a);
        }
    }

    // Every combination is validated here, before any work, so a refused run
    // costs nothing and an accepted one cannot be mislabelled (spec 20).
    if (o.dataset.empty()) invalid("--dataset is required");
    if (!o.have_source || !o.have_target) invalid("--source and --target are required");
    if (o.algorithm != "dijkstra" && o.algorithm != "astar") {
        invalid("unknown --algorithm: " + o.algorithm);
    }
    static const std::vector<std::string> kQueues = {
        "binary_heap", "four_ary_heap", "pairing_heap", "fibonacci_heap",
        "radix_heap",  "lpq",           "lpq_model",    "lpq_adaptive", "lpq_optimized",
        "lpq_online"};
    if (std::find(kQueues.begin(), kQueues.end(), o.pq) == kQueues.end()) {
        invalid("unknown --pq: " + o.pq);
    }
    const bool learned = o.pq.rfind("lpq", 0) == 0;
    if (o.algorithm == "dijkstra" && o.heuristic != "auto") {
        invalid("--heuristic only applies to --algorithm astar");
    }
    if (o.pq == "lpq_model" && o.model.empty()) {
        invalid("--pq lpq_model requires --model; without one it is the control condition, "
                "spelled --pq lpq");
    }
    if (!o.model.empty() && o.pq != "lpq_model") {
        invalid("--model only affects --pq lpq_model; with --pq " + o.pq +
                " it would be loaded and never consulted");
    }
    if (o.adaptive_learning && o.pq != "lpq_adaptive") {
        invalid("--adaptive-learning only affects --pq lpq_adaptive");
    }
    {
        const bool online_flags =
            !o.online_model.empty() || !o.online_report.empty() || o.allow_graph_mismatch ||
            o.allow_width_mismatch || o.online_no_provenance || o.online_start != "warm" ||
            o.online_update_mode != "periodic" || o.online_apply != "between_queries";
        if (o.pq != "lpq_online") {
            if (online_flags) invalid("--online-* and --allow-* model flags only affect --pq lpq_online");
        } else {
            if (o.online_start != "warm" && o.online_start != "cold") {
                invalid("--online-start expects warm or cold");
            }
            if (o.online_update_mode != "disabled" && o.online_update_mode != "every_event" &&
                o.online_update_mode != "periodic" && o.online_update_mode != "adaptive") {
                invalid("--online-update-mode expects disabled, every_event, periodic or adaptive");
            }
            if (o.online_apply != "between_queries") {
                invalid("--online-apply " + o.online_apply +
                        " is not implemented; only between_queries is");
            }
            if (o.online_start == "cold") {
                if (!o.online_model.empty() || !o.online_report.empty() || o.allow_graph_mismatch ||
                    o.allow_width_mismatch || o.online_no_provenance) {
                    invalid("a cold start has no artifact: --online-model, --online-model-report "
                            "and the --allow-* / no-provenance flags do not apply");
                }
            } else if (o.online_model.empty()) {
                invalid("--pq lpq_online needs --online-model <artifact>: there is no default "
                        "model, and a model trained for one graph is not assumed to suit another "
                        "(or --online-start cold for a zero-initialised model)");
            }
        }
    }
    if (!learned && (o.window_min != 2 || o.window_max != 32 || o.bucket_width != 0)) {
        invalid("--lpq-window and --lpq-bucket-width only apply to learned queues");
    }
    if (o.pq == "lpq_optimized" &&
        (o.mode != "exact" || o.window_min != 2 || o.window_max != 32 || o.bucket_width != 0)) {
        invalid("lpq_optimized is exact-only and chooses its own geometry; it takes no "
                "--mode approximate, --lpq-window or --lpq-bucket-width");
    }
    if (!o.policy.empty() && o.pq != "lpq_optimized") {
        invalid("--policy only affects --pq lpq_optimized");
    }
    if (o.mode != "exact" && o.mode != "approximate") invalid("unknown --mode: " + o.mode);
    if (o.mode == "approximate") {
        if (!learned) {
            invalid(o.pq + " has no approximation mechanism; approximate mode would return "
                           "exact results under an approximate label");
        }
        if (!(o.epsilon > 0.0) || o.epsilon > 1.0) {
            invalid("--mode approximate requires 0 < --epsilon <= 1");
        }
    } else if (o.epsilon != 0.0) {
        invalid("--epsilon is meaningless in exact mode");
    }
    if (o.trace_level != TraceLevel::kNone && o.trace_out.empty()) {
        invalid("--trace-out is required when --trace-level is not none");
    }
    if (o.repetitions > 1000 || o.warmup > 100) {
        invalid("--repetitions must be <= 1000 and --warmup <= 100");
    }
    if (o.max_events == 0 || o.max_events > 5000000) {
        invalid("--max-events must be between 1 and 5000000");
    }
    return o;
}

// -----------------------------------------------------------------------------
// The search, dispatched once on algorithm and heuristic
// -----------------------------------------------------------------------------

struct Context {
    const Graph*         g = nullptr;
    GraphMetadata        meta;
    Options              o;
    std::string          heuristic;  ///< resolved; empty for dijkstra
    double               heuristic_scale = 1.0;
    const Scorer*        scorer          = nullptr;
    OnlineSession*       online          = nullptr;  ///< lpq_online: the live model and learner
    struct OnlineRunInfo {
        std::string              start = "warm";
        std::string              artifact_name;
        std::uint64_t            artifact_fnv1a64 = 0;
        std::uint64_t            artifact_bytes   = 0;
        std::string              report_name;
        std::optional<ModelReport> report;
        OnlineModelCompat        compat;
        std::uint64_t            graph_checksum = 0;
        std::uint64_t            bucket_width   = 0;
    } online_info;

    // lpq_optimized: what the queue policy resolved to, and what deciding cost.
    QueueChoice          choice;
    WeightProfile        profile;
    ProfileSource        profile_source = ProfileSource::kComputed;
    std::string          policy_source;
    std::uint64_t        decide_ns = 0;  ///< policy evaluation only (profile is separate)
    std::string          resolution_method;  ///< autotune | autotune_cached | policy | default
    std::uint64_t        calibration_ns = 0; ///< autotune measurement cost (0 when cached)
    std::vector<AutotuneEntry> tuned;        ///< every candidate the autotuner measured
};

template <typename Queue, typename Obs>
ShortestPathResult<W> search(const Context& c, Queue& queue, Obs* obs) {
    const auto s = static_cast<NodeId>(c.o.source);
    const auto t = static_cast<NodeId>(c.o.target);
    if (c.o.algorithm == "dijkstra") {
        DijkstraOptions d;
        d.target       = t;
        d.measure_time = false;
        return dijkstra<W, Queue, Obs>(*c.g, s, queue, d, obs);
    }
    AstarOptions a;
    a.target       = t;
    a.measure_time = false;
    if (c.heuristic == "zero") {
        ZeroHeuristic<W> h;
        return astar<W, Queue, ZeroHeuristic<W>, Obs>(*c.g, s, queue, h, a, obs);
    }
    if (c.heuristic == "euclidean") {
        EuclideanHeuristic<W> h(c.heuristic_scale);
        return astar<W, Queue, EuclideanHeuristic<W>, Obs>(*c.g, s, queue, h, a, obs);
    }
    if (c.heuristic == "manhattan") {
        ManhattanHeuristic<W> h(c.heuristic_scale);
        return astar<W, Queue, ManhattanHeuristic<W>, Obs>(*c.g, s, queue, h, a, obs);
    }
    HaversineHeuristic<W> h(c.heuristic_scale);
    return astar<W, Queue, HaversineHeuristic<W>, Obs>(*c.g, s, queue, h, a, obs);
}

template <typename Queue>
constexpr bool kIsLearned = std::is_same_v<Queue, LpqPlain> || std::is_same_v<Queue, LpqStatic> ||
                            std::is_same_v<Queue, LpqOnline>;

template <typename Queue>
Queue make_queue(const Context& c) {
    if constexpr (kIsLearned<Queue>) {
        CandidateWindowConfig window;
        window.k_min = static_cast<std::uint32_t>(c.o.window_min);
        window.k_max = static_cast<std::uint32_t>(c.o.window_max);
        const auto width =
            derive_bucket_width(c.meta, Queue::Substrate::kBucketCount, c.o.bucket_width);
        const CorrectnessPolicy policy = c.o.mode == "approximate"
                                             ? CorrectnessPolicy::approximate(c.o.epsilon)
                                             : CorrectnessPolicy::exact();
        Queue q{static_cast<Dist>(width), window, policy};
        if constexpr (std::is_same_v<Queue, LpqStatic>) {
            q.set_scorer(c.scorer);
        }
        if constexpr (std::is_same_v<Queue, LpqOnline>) {
            q.set_scorer(&c.online->scorer());
        }
        return q;
    } else if constexpr (std::is_same_v<Queue, PooledBucketQueue<W>>) {
        return Queue{static_cast<Dist>(c.choice.width), c.choice.ring};
    } else {
        return Queue{};
    }
}

/// What the learner did in a run (D-12). Attachment, recorded events, applied updates and a
/// measured change of the weights are separate facts; none says the learner helped.
struct LearnerReport {
    bool                  attached = false;
    OnlineLearningMetrics m;
    bool                  has_model = false;  ///< a live model was updated (lpq_online only)
    bool                  weights_changed = false;
    bool                  halted = false;
    std::uint64_t         start_hash = 0;
    std::uint64_t         end_hash = 0;
    std::uint64_t         without_prediction = 0;
    std::size_t           buffer_memory_bytes = 0;
    bool                  accounting_balanced = true;
};

/// Everything one execution produced, gathered before any JSON is written.
struct Execution {
    std::optional<LearnerReport>       learner;
    ShortestPathResult<W> result;
    std::optional<LpqMetrics>          lpq;
    std::optional<CandidateMetrics>    candidate;
    std::optional<VerificationMetrics> verification;
    std::optional<CorrectionMetrics>   correction;
    std::uint64_t                      bucket_width = 0;

    std::optional<BenchmarkMeasurement> timing;

    bool          traced               = false;
    bool          trace_matches        = false;
    std::string   trace_mismatch;
    std::uint64_t trace_events_written = 0;
    std::uint64_t trace_events_dropped = 0;
    std::uint64_t trace_run_ns         = 0;
    std::map<std::string, std::uint64_t> trace_lpq_paths;
};

std::string compare_runs(const ShortestPathResult<W>& a, const ShortestPathResult<W>& b,
                         NodeId target) {
    if (a.termination != b.termination) return "termination differs";
    if (a.dist[target] != b.dist[target]) return "target distance differs";
    if (a.path_to(target) != b.path_to(target)) return "path differs";
    const auto& x = a.counters;
    const auto& y = b.counters;
    if (x.settled_nodes != y.settled_nodes || x.pq_pops != y.pq_pops ||
        x.pq_pushes != y.pq_pushes || x.relaxations != y.relaxations ||
        x.successful_relaxations != y.successful_relaxations || x.stale_pops != y.stale_pops) {
        return "operation counters differ";
    }
    return {};
}

template <typename Queue>
Execution execute(const Context& c) {
    Execution ex;
    const auto target = static_cast<NodeId>(c.o.target);

    // Adaptation state lives outside the queue. Reset before every execution
    // so the authoritative, traced and timed runs all start from the same
    // controller state, exactly as lpq_benchmark resets it per repetition.
    AdaptiveController controller{ControllerConfig{}};
    OnlineLearner      learner{OnlineLearningConfig{}};
    ControllerObserver adaptive(controller, c.o.adaptive_learning ? &learner : nullptr);
    const bool         is_adaptive = c.o.pq == "lpq_adaptive";
    const auto         reset_adaptation = [&] {
        controller.reset();
        learner.reset();
    };

    // --- 1. authoritative run ---------------------------------------------
    Queue queue = make_queue<Queue>(c);
    queue.reserve_nodes(c.g->num_nodes());
    if constexpr (kIsLearned<Queue>) {
        if (is_adaptive) queue.set_observer(&adaptive);
        if constexpr (std::is_same_v<Queue, LpqOnline>) {
            // Only the authoritative run records events; the traced and timed runs below must not
            // add to what the learner sees.
            queue.set_observer(&c.online->observer());
        }
        ex.bucket_width = derive_bucket_width(c.meta, Queue::Substrate::kBucketCount,
                                              c.o.bucket_width);
        queue.reset_metrics();
        reset_adaptation();
    }
    ex.result = search<Queue, NullSearchObserver>(c, queue, nullptr);
    if constexpr (kIsLearned<Queue>) {
        if (is_adaptive) {
            // lpq_adaptive's learner only ever records. Captured now: the next reset clears it.
            LearnerReport lr;
            lr.attached            = c.o.adaptive_learning;
            lr.m                   = learner.metrics();
            lr.accounting_balanced = learner.accounting_balanced();
            lr.buffer_memory_bytes = c.o.adaptive_learning ? learner.memory_bytes() : 0;
            ex.learner             = lr;
        } else if constexpr (!std::is_same_v<Queue, LpqOnline>) {
            ex.learner = LearnerReport{};  // lpq and lpq_model have no learner
        }
        if constexpr (std::is_same_v<Queue, LpqOnline>) {
            queue.set_observer(nullptr);
        }
        ex.lpq          = queue.metrics();
        ex.candidate    = queue.candidate_metrics();
        ex.verification = queue.verification_metrics();
        ex.correction   = queue.correction_metrics();
    }

    // --- 2. traced run ------------------------------------------------------
    if (c.o.trace_level != TraceLevel::kNone) {
        std::ofstream out(c.o.trace_out, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw CategorisedError("io", "cannot open trace output");
        }
        TraceRecorder rec(out, c.o.trace_level, c.o.max_events, c.o.algorithm == "astar");
        Queue         tq = make_queue<Queue>(c);
        tq.reserve_nodes(c.g->num_nodes());
        if constexpr (kIsLearned<Queue>) {
            rec.attach_lpq(
                [&tq](LpqSnapshot& s, std::size_t cap) {
                    auto& sub    = tq.substrate();
                    s.queue_size = sub.size();
                    if (sub.size() == 0) return;
                    s.bucket_size  = sub.current_bucket_size();
                    const auto f   = sub.current_front();
                    s.bucket_base  = static_cast<double>(sub.base());
                    s.bucket_width = static_cast<double>(sub.width());
                    s.bound        = static_cast<double>(sub.current_bound());
                    for (std::size_t i = 0; i < f.size() && i < cap; ++i) {
                        s.front.push_back({f[i].node, static_cast<double>(f[i].key), f[i].score});
                    }
                },
                is_adaptive ? &adaptive : nullptr);
            tq.set_observer(&rec);
            tq.reset_metrics();
            reset_adaptation();
        }
        rec.run_started(c.o.algorithm == "astar" ? "astar:" + c.heuristic : c.o.algorithm, c.o.pq,
                        static_cast<NodeId>(c.o.source), target);
        const auto t0     = std::chrono::steady_clock::now();
        const auto traced = search<Queue, TraceRecorder>(c, tq, &rec);
        ex.trace_run_ns   = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
                .count());
        const bool reached = traced.reachable(target);
        if (reached) {
            rec.path_reconstructed(traced.path_to(target), static_cast<double>(traced.dist[target]));
        }
        rec.run_completed(to_string(traced.termination), reached,
                          reached ? static_cast<double>(traced.dist[target]) : -1.0);
        ex.traced         = true;
        ex.trace_mismatch = compare_runs(ex.result, traced, target);
        if constexpr (kIsLearned<Queue>) {
            const auto& m = tq.metrics();
            if (ex.trace_mismatch.empty() &&
                (m.extractions != ex.lpq->extractions ||
                 m.verified_extractions != ex.lpq->verified_extractions ||
                 m.fallback_scans != ex.lpq->fallback_scans)) {
                ex.trace_mismatch = "learned-queue metrics differ";
            }
        }
        ex.trace_matches        = ex.trace_mismatch.empty();
        ex.trace_events_written = rec.events_written();
        ex.trace_events_dropped = rec.events_dropped();
        ex.trace_lpq_paths      = rec.lpq_paths();
        if (!out) {
            throw CategorisedError("io", "failed while writing the trace");
        }
    }

    // --- 3. timed runs ------------------------------------------------------
    if (c.o.repetitions > 0) {
        BenchmarkConfig cfg;
        cfg.repetitions        = c.o.repetitions;
        cfg.warmup_repetitions = c.o.warmup;
        cfg.cpu_core           = c.o.cpu_core;
        ex.timing              = run_benchmark(
            [&] {
                if constexpr (kIsLearned<Queue>) {
                    if (is_adaptive) reset_adaptation();
                }
                const auto r = search<Queue, NullSearchObserver>(c, queue, nullptr);
                do_not_optimize(r.dist[target]);
            },
            cfg);
    }

    if constexpr (std::is_same_v<Queue, LpqOnline>) {
        // Between-query update: the search above (and the traced and timed repeats) ran on the
        // start model; only now are the recorded events applied.
        OnlineSession& session = *c.online;
        (void)session.end_query();
        LearnerReport lr;
        lr.attached            = true;
        lr.m                   = session.learner().metrics();
        lr.has_model           = true;
        lr.halted              = session.learner().halted();
        lr.start_hash          = session.start_hash();
        lr.end_hash            = session.current_hash();
        lr.weights_changed     = session.weights_changed();
        lr.without_prediction  = session.observer().extractions_without_prediction();
        lr.buffer_memory_bytes = session.learner().memory_bytes();
        lr.accounting_balanced = session.learner().accounting_balanced();
        ex.learner             = lr;
    }
    return ex;
}

// -----------------------------------------------------------------------------
// JSON output
// -----------------------------------------------------------------------------

const char* coordinate_system_name(CoordinateSystem s) {
    switch (s) {
        case CoordinateSystem::kNone: return "none";
        case CoordinateSystem::kEuclidean: return "euclidean";
        case CoordinateSystem::kGeographic: return "geographic";
    }
    return "unknown";
}

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

/// The measured status of the online learner (D-12). `online_learning` elsewhere in the result is
/// derived from this block: true only when updates were applied and the weights changed.
void write_learner(JsonWriter& w, const Context& c, const LearnerReport& lr) {
    const auto& m = lr.m;
    w.begin_object("learner");
    w.key_value("schema", static_cast<std::int64_t>(1));
    w.key_value("attached", lr.attached);
    w.key_value("events_recorded", m.events_recorded);
    w.key_value("examples_recorded", m.examples_recorded);
    w.key_value("events_applied", m.events_applied);
    w.key_value("examples_applied", m.examples_applied);
    w.key_value("events_dropped", m.events_dropped);
    w.key_value("events_rolled_back", m.events_rolled_back);
    w.key_value("events_discarded_disabled", m.events_discarded_disabled);
    w.key_value("events_discarded_halted", m.events_discarded_halted);
    w.key_value("updates_applied", m.updates_performed);
    w.key_value("largest_batch", m.largest_batch);
    w.key_value("rollbacks", m.rollbacks);
    w.key_value("halted", lr.halted);
    w.key_value("accounting_balanced", lr.accounting_balanced);
    w.key_value("extractions_without_prediction", lr.without_prediction);
    w.key_value("buffer_memory_bytes", static_cast<std::uint64_t>(lr.buffer_memory_bytes));
    w.key_value("weights_changed", lr.weights_changed);
    if (lr.has_model) {
        w.key_value("weights_hash_start", hex64(lr.start_hash));
        w.key_value("weights_hash_end", hex64(lr.end_hash));
    } else {
        w.key_null("weights_hash_start");
        w.key_null("weights_hash_end");
    }
    if (c.o.pq == "lpq_online") {
        const auto& oi = c.online_info;
        w.key_value("evaluation", "single_query_session");
        w.key_value("apply_policy", c.o.online_apply);
        w.key_value("update_mode", c.o.online_update_mode);
        w.key_value("start", oi.start);
        w.key_value("label_definition", "online_first_choice_extractable_v1");
        w.key_value("note",
                    "Updates are applied after the search, so the search ran on the start model. "
                    "An applied update is not an improvement: nothing here measures whether the "
                    "model got better.");
        w.begin_object("model");
        if (oi.start == "cold") {
            w.key_value("kind", "logistic");
            w.key_value("source", "zero_initialised");
        } else {
            w.key_value("kind", "logistic");
            w.key_value("source", "artifact");
            w.key_value("artifact_name", oi.artifact_name);
            w.key_value("artifact_bytes", oi.artifact_bytes);
            w.key_value("artifact_fnv1a64", hex64(oi.artifact_fnv1a64));
            w.key_value("hash_note",
                        "FNV-1a identifies these bytes; it is not a digest. The SHA-256 is recorded "
                        "by the caller.");
            w.key_value("feature_layout_version", static_cast<std::int64_t>(kFeatureLayoutVersion));
            w.key_value("feature_count", static_cast<std::uint64_t>(kFeatureCount));
            w.begin_object("compatibility");
            w.key_value("provenance", oi.compat.provenance);
            w.key_value("graph_match", oi.compat.graph_match);
            w.key_value("width_match", oi.compat.width_match);
            w.key_value("run_graph_checksum", std::to_string(oi.graph_checksum));
            w.key_value("run_bucket_width", oi.bucket_width);
            w.begin_array("overrides");
            for (const auto& o : oi.compat.overrides) w.value(std::string_view(o));
            w.end_array();
            w.key_value("suitability", oi.compat.suitability);
            w.end_object();
            if (oi.report) {
                const ModelReport& r = *oi.report;
                w.begin_object("report");
                w.key_value("file", oi.report_name);
                w.key_value("dataset_id", r.dataset_id);
                if (r.graph_checksum) w.key_value("graph_checksum", std::to_string(*r.graph_checksum));
                else w.key_null("graph_checksum");
                if (r.bucket_width) w.key_value("bucket_width", *r.bucket_width);
                else w.key_null("bucket_width");
                if (r.query_seed) w.key_value("query_seed", *r.query_seed);
                else w.key_null("query_seed");
                if (r.query_count) w.key_value("query_count", *r.query_count);
                else w.key_null("query_count");
                if (r.manifest_name.empty()) {
                    w.key_null("training_manifest");
                } else {
                    w.begin_object("training_manifest");
                    w.key_value("name", r.manifest_name);
                    w.key_value("sha256", r.manifest_sha256);
                    w.key_value("selection", r.manifest_selection);
                    w.end_object();
                }
                w.end_object();
            } else {
                w.key_null("report");
            }
        }
        w.end_object();
    } else if (lr.attached) {
        w.key_value("label_definition", "legacy_events_without_features");
        w.key_value("note",
                    "lpq_adaptive records one event per extraction with empty features, and no "
                    "model exists to update, so nothing is learned.");
    }
    w.end_object();
}

void write_environment(JsonWriter& w, const HardwareInfo& hw) {
    w.begin_object("environment");
    w.key_value("cpu_model", hw.cpu_model);
    w.key_value("logical_cores", static_cast<std::uint64_t>(hw.logical_cores));
    w.key_value("os", hw.os);
    w.key_value("compiler", hw.compiler);
    w.key_value("compiler_version", hw.compiler_version);
    w.key_value("build_type", hw.build_type);
    w.key_value("optimization_flags", hw.optimization_flags);
    w.key_value("instrumentation_enabled", hw.instrumentation_enabled);
    w.key_value("native_arch", hw.native_arch);
    w.key_value("clock_resolution_ns", hw.clock_resolution_ns);
    w.key_value("git_commit", hw.git_commit);
    w.key_value("git_dirty", hw.git_dirty);
    w.end_object();
    w.begin_object("hardware_counters");
    w.key_value("available", false);
    w.key_value("reason", hw.hardware_counters_available
                              ? std::string("not collected by lpq_run")
                              : std::string("Unavailable on this execution environment (no CPU "
                                            "PMU exposed); never estimated"));
    w.end_object();
}

void write_result(std::ostream& os, const Context& c, const Execution& ex,
                  const ShortestPathResult<W>& ref, const HardwareInfo& hw,
                  std::uint64_t load_ns, std::uint64_t scale_ns, std::uint64_t scorer_ns,
                  std::uint64_t ref_ns, std::uint64_t checksum) {
    const auto  target    = static_cast<NodeId>(c.o.target);
    const auto& r         = ex.result;
    const bool  reachable = r.reachable(target);
    const bool  ref_reach = ref.reachable(target);
    const bool  learned   = c.o.pq.rfind("lpq", 0) == 0;

    JsonWriter w(os, true);
    w.begin_object();
    w.key_value("schema_version", static_cast<std::int64_t>(kRunSchemaVersion));
    w.key_value("run_id", c.o.run_id);
    w.key_value("status", "completed");
    w.key_value("generated_at", iso8601_now());

    w.begin_object("request");
    w.key_value("algorithm", c.o.algorithm);
    if (c.heuristic.empty()) w.key_null("heuristic");
    else w.key_value("heuristic", c.heuristic);
    w.key_value("queue", c.o.pq);
    w.key_value("mode", c.o.mode);
    w.key_value("epsilon", c.o.epsilon);
    w.key_value("source", c.o.source);
    w.key_value("target", c.o.target);
    w.key_value("trace_level", trace_level_name(c.o.trace_level));
    w.key_value("max_events", c.o.max_events);
    w.key_value("repetitions", static_cast<std::uint64_t>(c.o.repetitions));
    w.key_value("warmup", static_cast<std::uint64_t>(c.o.warmup));
    w.key_value("adaptive_learning", c.o.adaptive_learning);
    w.end_object();

    w.begin_object("dataset");
    w.key_value("id", std::filesystem::path(c.o.dataset).stem().string());
    w.key_value("nodes", static_cast<std::uint64_t>(c.meta.node_count));
    w.key_value("edges", static_cast<std::uint64_t>(c.meta.edge_count));
    w.key_value("arcs", static_cast<std::uint64_t>(c.meta.arc_count));
    w.key_value("directed", c.meta.directedness == Directedness::kDirected);
    // As a string: a 64-bit checksum does not survive a JavaScript number.
    w.key_value("checksum_fnv1a64", std::to_string(checksum));
    w.key_value("coordinate_system", coordinate_system_name(c.g->coordinate_system()));
    w.key_value("min_weight", c.meta.min_weight);
    w.key_value("max_weight", c.meta.max_weight);
    w.end_object();

    w.begin_object("result");
    w.key_value("reachable", reachable);
    w.key_value("termination", to_string(r.termination));
    if (reachable) {
        const auto path = r.path_to(target);
        w.key_value("distance", static_cast<std::uint64_t>(r.dist[target]));
        w.begin_array("path");
        for (const NodeId v : path) w.value(static_cast<std::uint64_t>(v));
        w.end_array();
        // The weight of each hop, taken from the graph -- the cheapest arc
        // between consecutive path nodes, which is the one a shortest path uses.
        w.begin_array("path_weights");
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
            for (const lpq::Arc<W>& a : c.g->out_arcs(path[i])) {
                if (a.target == path[i + 1]) best = std::min<std::uint64_t>(best, a.weight);
            }
            w.value(best);
        }
        w.end_array();
        w.key_value("path_hops", static_cast<std::uint64_t>(path.empty() ? 0 : path.size() - 1));
        w.key_value("path_cost_verified", r.verify_path_cost(*c.g, path) == r.dist[target]);
    } else {
        w.key_null("distance");
        w.begin_array("path");
        w.end_array();
        w.begin_array("path_weights");
        w.end_array();
        w.key_value("path_hops", static_cast<std::uint64_t>(0));
        w.key_value("path_cost_verified", false);
    }
    w.end_object();

    w.begin_object("reference");
    w.key_value("algorithm", "reference_dijkstra_heap");
    w.key_value("reachable", ref_reach);
    if (ref_reach) w.key_value("distance", static_cast<std::uint64_t>(ref.dist[target]));
    else w.key_null("distance");
    w.end_object();

    w.begin_object("correctness");
    w.key_value("policy", c.o.mode);
    w.key_value("epsilon", c.o.epsilon);
    w.key_value("reachability_agrees", reachable == ref_reach);
    if (reachable && ref_reach) {
        const double ratio = approximation_ratio<W>(r.dist[target], ref.dist[target]);
        w.key_value("exact_match", costs_equal<W>(r.dist[target], ref.dist[target]));
        w.key_value("approximation_ratio", ratio);
        w.key_value("bound", 1.0 + c.o.epsilon);
        w.key_value("bound_holds", c.o.mode == "approximate"
                                       ? within_epsilon<W>(r.dist[target], ref.dist[target],
                                                           c.o.epsilon)
                                       : costs_equal<W>(r.dist[target], ref.dist[target]));
    } else {
        // Unreachable pairs have no ratio. A number here would be invented.
        w.key_value("exact_match", reachable == ref_reach);
        w.key_null("approximation_ratio");
        w.key_value("bound", 1.0 + c.o.epsilon);
        w.key_value("bound_holds", reachable == ref_reach);
    }
    w.end_object();

    w.key_value("counters_available", kInstrumentationEnabled);
    if (kInstrumentationEnabled) {
        const auto& k = r.counters;
        w.begin_object("counters");
        w.key_value("pq_pushes", k.pq_pushes);
        w.key_value("pq_pops", k.pq_pops);
        w.key_value("pq_decrease_keys", k.pq_decrease_keys);
        w.key_value("pq_comparisons", k.pq_comparisons);
        w.key_value("stale_pops", k.stale_pops);
        w.key_value("settled_nodes", k.settled_nodes);
        w.key_value("edges_examined", k.relaxations);
        w.key_value("edges_relaxed", k.successful_relaxations);
        w.key_value("peak_queue_size", k.peak_queue_size);
        w.end_object();
    } else {
        w.key_null("counters");
    }

    if (c.heuristic.empty()) {
        w.key_null("heuristic");
    } else {
        w.begin_object("heuristic");
        w.key_value("name", c.heuristic);
        w.key_value("scale", c.heuristic_scale);
        w.key_value("scale_derivation",
                    c.heuristic == "zero"        ? "none"
                    : c.heuristic == "haversine" ? "min over arcs of weight / great-circle metres"
                                                 : "min over arcs of weight / coordinate length");
        w.key_value("consistent", true);
        w.end_object();
    }

    if (c.o.pq == "lpq_optimized") {
        // What the optimised queue actually was in this run, and what choosing
        // it cost. When resolved_queue is the radix heap this is a classical
        // heap's result and must be read as one.
        w.begin_object("optimizer");
        w.key_value("resolved_queue", c.choice.describe());
        w.key_value("resolved_kind", c.choice.kind == QueueKind::kRadixHeap ? "radix_heap"
                                                                            : "pooled_bucket");
        w.key_value("bucket_width", c.choice.kind == QueueKind::kPooledBucket ? c.choice.width : 0);
        w.key_value("ring_size",
                    static_cast<std::uint64_t>(c.choice.kind == QueueKind::kPooledBucket ? c.choice.ring
                                                                                         : 0));
        w.key_value("reason", c.choice.reason);
        w.key_value("predicted_ratio_vs_radix", c.choice.predicted_ratio);
        w.key_value("policy_source", c.policy_source);
        w.key_value("resolution_method", c.resolution_method);
        w.key_value("calibration_ns", c.calibration_ns);
        w.begin_array("tuned_candidates");
        for (const auto& e : c.tuned) {
            w.begin_object();
            w.key_value("bucket_width", e.width);  // 0 = the radix heap, the reference
            w.key_value("geomean_ratio_vs_radix", e.geomean_ratio);
            w.key_value("win_share", e.win_share);
            w.end_object();
        }
        w.end_array();
        w.key_value("profile_source", std::string(to_string(c.profile_source)));
        w.key_value("profile_ns", c.profile.compute_ns);
        w.key_value("decision_ns", c.decide_ns);
        w.key_value("per_query_timing_includes_setup", false);
        w.end_object();
    }

    if (learned && ex.lpq) {
        w.begin_object("lpq");
        w.key_value("bucket_width", ex.bucket_width);
        w.begin_array("window");
        w.value(static_cast<std::uint64_t>(c.o.window_min));
        w.value(static_cast<std::uint64_t>(c.o.window_max));
        w.end_array();
        // D-02: say exactly what the learned component is in this run.
        w.key_value("scorer", c.o.pq == "lpq_model"
                                  ? "static_precomputed_model_scores"
                              : c.o.pq == "lpq_online" ? "online_logistic_static_features_on_demand"
                                                       : "constant_control_no_model");
        w.key_value("adaptive_controller", c.o.pq == "lpq_adaptive");
        // Measured, not configured: true only when updates were applied and the weights differ
        // from where they started. (Until M5-O1 this read "a learner was attached", which could
        // be true while no weight ever changed.)
        const LearnerReport none{};
        const LearnerReport& lr = ex.learner ? *ex.learner : none;
        w.key_value("online_learning", lr.m.updates_performed > 0 && lr.weights_changed);
        write_learner(w, c, lr);
        if (c.o.pq == "lpq_model") {
            w.key_value("model_path_name", std::filesystem::path(c.o.model).filename().string());
            w.key_value("scorer_build_ns", scorer_ns);
            w.key_value("scorer_memory_bytes", static_cast<std::uint64_t>(c.scorer->memory_bytes()));
            w.begin_array("features");
            for (const char* f : {"out_degree", "min_adjacent_weight", "mean_adjacent_weight",
                                  "max_adjacent_weight", "adjacent_weight_spread",
                                  "degree_relative_to_mean"}) {
                w.value(std::string_view(f));
            }
            w.end_array();
        }
        const auto& m = *ex.lpq;
        w.begin_object("metrics");
        w.key_value("extractions", m.extractions);
        w.key_value("verified_extractions", m.verified_extractions);
        w.key_value("fallback_scans", m.fallback_scans);
        w.key_value("entries_examined", m.entries_examined);
        w.key_value("entries_skipped", m.entries_skipped);
        w.key_value("window_divergences", m.window_divergences);
        w.key_value("verified_rate", m.verified_rate());
        w.key_value("fallback_rate", m.extractions == 0 ? 0.0
                                                        : static_cast<double>(m.fallback_scans) /
                                                              static_cast<double>(m.extractions));
        w.key_value("mean_entries_skipped", m.mean_entries_skipped());
        w.end_object();
        const auto& cm = *ex.candidate;
        w.begin_object("candidate");
        w.key_value("windows_opened", cm.extractions);
        w.key_value("offered", cm.candidates_offered);
        w.key_value("admitted", cm.candidates_admitted);
        w.key_value("rejected_by_score", cm.candidates_rejected);
        w.key_value("expansions", cm.expansions);
        w.key_value("expansions_exhausted", cm.expansions_exhausted);
        w.key_value("mean_window_size", cm.mean_window_size());
        w.end_object();
        const auto& vm = *ex.verification;
        w.begin_object("verification");
        w.key_value("verifications", vm.verifications);
        w.key_value("accepted", vm.accepted);
        w.key_value("window_too_narrow", vm.window_too_narrow);
        w.key_value("budget_exhausted", vm.budget_exhausted);
        w.key_value("comparisons", vm.comparisons);
        w.end_object();
        const auto& co = *ex.correction;
        w.begin_object("correction");
        w.key_value("extractions", co.extractions);
        w.key_value("first_attempt", co.first_attempt);
        w.key_value("first_attempt_corrected", co.first_attempt_corrected);
        w.key_value("expanded", co.expanded);
        w.key_value("fallbacks", co.fallbacks);
        w.key_value("empty", co.empty);
        w.key_value("total_comparisons", co.total_comparisons);
        w.end_object();
        w.end_object();
    } else {
        w.key_null("lpq");
    }

    w.begin_object("timing");
    w.key_value("clock_boundary",
                "graph loaded, queue allocated and sized, heuristic scale derived and model "
                "scores precomputed BEFORE the clock; one untraced search per sample; no "
                "output serialization inside the timed region");
    if (ex.timing) {
        const auto& t = *ex.timing;
        w.begin_object("benchmark");
        w.key_value("repetitions", static_cast<std::uint64_t>(t.samples_ns.size()));
        w.key_value("warmup", static_cast<std::uint64_t>(t.warmup_ns.size()));
        w.key_array("samples_ns", t.samples_ns);
        w.key_value("median_ns", t.summary.median);
        w.key_value("mad_ns", t.summary.mad);
        w.key_value("min_ns", t.summary.min);
        w.key_value("max_ns", t.summary.max);
        w.key_value("relative_mad", t.summary.relative_mad());
        w.key_value("noisy", t.noisy);
        w.key_value("pinned", t.affinity.pinned);
        w.key_value("core", t.affinity.core);
        w.begin_array("warnings");
        for (const auto& s : t.warnings) w.value(std::string_view(s));
        w.end_array();
        w.end_object();
    } else {
        w.key_null("benchmark");
    }
    w.begin_object("phases_ns");
    w.key_value("graph_load", load_ns);
    w.key_value("heuristic_scale", scale_ns);
    w.key_value("scorer_build", scorer_ns);
    w.key_value("reference_search", ref_ns);
    if (ex.traced) w.key_value("instrumented_trace_run", ex.trace_run_ns);
    else w.key_null("instrumented_trace_run");
    w.end_object();
    w.end_object();

    w.begin_object("trace");
    w.key_value("level", trace_level_name(c.o.trace_level));
    w.key_value("available", ex.traced);
    if (ex.traced) {
        w.key_value("events_written", ex.trace_events_written);
        w.key_value("events_dropped", ex.trace_events_dropped);
        w.key_value("truncated", ex.trace_events_dropped > 0);
        w.key_value("matches_untraced_run", ex.trace_matches);
        if (!ex.trace_matches) w.key_value("mismatch", ex.trace_mismatch);
        w.key_value("schema_version", static_cast<std::int64_t>(kTraceSchemaVersion));
    }
    w.end_object();

    write_environment(w, hw);
    w.end_object();
    os << '\n';
}

void write_failure(std::ostream& os, const std::string& run_id, const std::string& category,
                   const std::string& message) {
    JsonWriter w(os, true);
    w.begin_object();
    w.key_value("schema_version", static_cast<std::int64_t>(kRunSchemaVersion));
    w.key_value("run_id", run_id);
    w.key_value("status", "failed");
    w.begin_object("error");
    w.key_value("category", category);
    w.key_value("message", message);
    w.end_object();
    w.end_object();
    os << '\n';
}

std::uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
}

int emit(const std::string& path, const std::string& text) {
    if (path.empty()) {
        std::cout << text;
        return 0;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string run_id = "adhoc";
    std::string result_out;
    try {
        Options o = parse_args(argc, argv);
        run_id     = o.run_id;
        result_out = o.result_out;

        Context c;
        c.o = o;

        auto              t0 = std::chrono::steady_clock::now();
        LoadedGraph<W>    loaded;
        try {
            loaded = read_binary_graph_file<W>(o.dataset);
        } catch (const std::exception& e) {
            throw CategorisedError("dataset", std::string("cannot load dataset: ") + e.what());
        }
        const std::uint64_t load_ns = ns_since(t0);
        const Graph&        g       = loaded.graph;
        c.g                         = &g;
        c.meta                      = compute_metadata(g);

        if (o.source >= g.num_nodes() || o.target >= g.num_nodes()) {
            invalid("source and target must be node ids below " + std::to_string(g.num_nodes()));
        }

        // --- heuristic ------------------------------------------------------
        std::uint64_t scale_ns = 0;
        if (o.algorithm == "astar") {
            std::string h = o.heuristic;
            if (h == "auto") {
                h = g.coordinate_system() == CoordinateSystem::kGeographic   ? "haversine"
                    : g.coordinate_system() == CoordinateSystem::kEuclidean ? "euclidean"
                                                                            : "";
                if (h.empty()) {
                    throw CategorisedError(
                        "unsupported",
                        "this dataset has no coordinates, so A* has no admissible geometric "
                        "heuristic; choose --heuristic zero or use dijkstra");
                }
            }
            if (h != "zero" && h != "euclidean" && h != "manhattan" && h != "haversine") {
                invalid("unknown --heuristic: " + h);
            }
            if (h != "zero") {
                if (!g.has_coordinates()) {
                    throw CategorisedError("unsupported", "--heuristic " + h +
                                                              " requires coordinates, which "
                                                              "this dataset does not have");
                }
                if (h == "haversine" && g.coordinate_system() != CoordinateSystem::kGeographic) {
                    throw CategorisedError("unsupported",
                                           "haversine requires geographic coordinates");
                }
                if (h != "haversine" && g.coordinate_system() != CoordinateSystem::kEuclidean) {
                    throw CategorisedError("unsupported",
                                           h + " requires plane coordinates; this dataset is "
                                               "geographic, use haversine");
                }
                t0                = std::chrono::steady_clock::now();
                c.heuristic_scale = h == "haversine" ? haversine_admissible_scale(g)
                                                     : euclidean_admissible_scale(g);
                scale_ns          = ns_since(t0);
                if (!(c.heuristic_scale > 0.0)) {
                    throw CategorisedError("unsupported",
                                           "no admissible heuristic scale exists for this graph (for haversine: coordinates must be degrees; reconvert files written before the degree conversion)");
                }
            }
            c.heuristic = h;
        }

        // --- model ----------------------------------------------------------
        std::unique_ptr<Scorer> scorer;
        std::uint64_t           scorer_ns = 0;
        if (o.pq == "lpq_model") {
            std::ifstream in(o.model, std::ios::binary);
            if (!in) {
                throw CategorisedError("model", "cannot open the model file");
            }
            t0 = std::chrono::steady_clock::now();
            std::optional<LogisticPredictor> predictor;
            try {
                predictor.emplace(LogisticPredictor::load(in));
            } catch (const std::exception& e) {
                throw CategorisedError("model", std::string("model rejected: ") + e.what());
            }
            const GraphFeatures features(g, c.meta);
            scorer = std::make_unique<Scorer>();
            scorer->build(features, *predictor);
            scorer_ns = ns_since(t0);
            c.scorer  = scorer.get();
        }

        // --- lpq_online: validate the warm-start artifact before any query runs --------------
        //
        // Loading proves only that the container is well formed. check_online_model adds what the
        // loader cannot know: that the weights are finite and sit on static features, and that the
        // report travelling with the artifact names this graph and this bucket width. An override
        // is recorded in the result, and a model that passes is still reported as "not assessed".
        std::unique_ptr<GraphFeatures> online_features;
        std::unique_ptr<OnlineSession> online_session;
        if (o.pq == "lpq_online") {
            t0 = std::chrono::steady_clock::now();
            Context::OnlineRunInfo& oi = c.online_info;
            oi.start                   = o.online_start;
            oi.graph_checksum          = loaded.checksum;
            oi.bucket_width            = derive_bucket_width(
                c.meta, LpqOnline::Substrate::kBucketCount, o.bucket_width);

            LogisticPredictor start_model;
            if (o.online_start == "warm") {
                std::ifstream in(o.online_model, std::ios::binary);
                if (!in) {
                    throw CategorisedError("model", "cannot open the model file");
                }
                std::ostringstream buffer;
                buffer << in.rdbuf();
                const std::string bytes = buffer.str();
                oi.artifact_name        = std::filesystem::path(o.online_model).filename().string();
                oi.artifact_bytes       = bytes.size();
                std::uint64_t h         = 14695981039346656037ULL;
                for (const char byte : bytes) {
                    h ^= static_cast<unsigned char>(byte);
                    h *= 1099511628211ULL;
                }
                oi.artifact_fnv1a64 = h;
                try {
                    std::istringstream is(bytes);
                    start_model = LogisticPredictor::load(is);
                } catch (const std::exception& e) {
                    throw CategorisedError("model", std::string("model rejected: ") + e.what());
                }

                std::optional<ModelReport> report;
                if (!o.online_report.empty()) {
                    std::ifstream rin(o.online_report, std::ios::binary);
                    if (!rin) {
                        throw CategorisedError("model", "cannot open the model report");
                    }
                    std::ostringstream rbuf;
                    rbuf << rin.rdbuf();
                    try {
                        report = parse_model_report(rbuf.str());
                    } catch (const ModelReportError& e) {
                        throw CategorisedError("model", e.what());
                    }
                    oi.report_name = std::filesystem::path(o.online_report).filename().string();
                }
                OnlineModelPolicy policy;
                policy.allow_graph_mismatch = o.allow_graph_mismatch;
                policy.allow_width_mismatch = o.allow_width_mismatch;
                policy.allow_no_provenance  = o.online_no_provenance;
                try {
                    oi.compat = check_online_model(start_model, report, oi.graph_checksum,
                                                   oi.bucket_width, policy);
                } catch (const OnlineModelError& e) {
                    throw CategorisedError("model", e.what());
                }
                oi.report = report;
            }

            OnlineLearningConfig lc;
            lc.mode = o.online_update_mode == "disabled"      ? UpdateMode::kDisabled
                      : o.online_update_mode == "every_event" ? UpdateMode::kEveryEvent
                      : o.online_update_mode == "adaptive"    ? UpdateMode::kAdaptive
                                                              : UpdateMode::kPeriodic;
            lc.start = o.online_start == "cold" ? StartPolicy::kCold : StartPolicy::kWarm;
            online_features = std::make_unique<GraphFeatures>(g, c.meta);
            online_session  = std::make_unique<OnlineSession>(*online_features, start_model, lc);
            c.online        = online_session.get();
            scorer_ns       = ns_since(t0);
        }

        // --- optimised queue: profile the graph, ask the policy ----------------
        if (o.pq == "lpq_optimized") {
            const std::string cache =
                std::filesystem::path(o.dataset).replace_extension(".lpqprofile").string();
            c.profile = load_or_compute_profile(cache, loaded.checksum, g, c.meta,
                                                c.profile_source);
            const std::uint64_t step = static_cast<std::uint64_t>(
                o.algorithm == "astar" ? 2.0 * c.profile.max_weight : c.profile.max_weight);
            t0 = std::chrono::steady_clock::now();
            if (!o.policy.empty()) {
                // An explicit policy file wins over autotuning.
                QueueSelectionPolicy policy;
                try {
                    policy = QueueSelectionPolicy::load(o.policy);
                } catch (const std::exception& e) {
                    throw CategorisedError("model", std::string("queue policy rejected: ") + e.what());
                }
                c.policy_source     = policy.source();
                c.choice            = policy.choose(c.profile, step);
                c.resolution_method = "policy";
            } else if (o.autotune && o.algorithm == "dijkstra") {
                const auto     hw0     = collect_hardware_info();
                const AutotuneConfig tcfg;
                const std::uint64_t  machine = machine_key(hw0.cpu_model, hw0.logical_cores);
                const std::string    tune_cache =
                    std::filesystem::path(o.dataset).replace_extension(".lpqtune").string();
                AutotuneResult tuned;
                if (autotune_cache_load(tune_cache, loaded.checksum, machine, o.algorithm, tcfg,
                                        tuned)) {
                    c.resolution_method = "autotune_cached";
                } else {
                    tuned = QueueAutotuner<W>(tcfg).tune(g, c.profile, step);
                    autotune_cache_store(tune_cache, tuned, loaded.checksum, machine, o.algorithm, tcfg);
                    c.resolution_method = "autotune";
                    c.calibration_ns    = tuned.calibration_ns;
                }
                c.choice        = tuned.choice;
                c.choice.reason = tuned.reason;
                c.tuned         = tuned.measured;
                c.policy_source = "autotune";
            } else {
                c.choice.reason = o.algorithm == "astar"
                    ? "A* resolves to the radix heap: pooled bucket queues were measured on three "
                      "road graphs and did not beat it under A*"
                    : "autotuning is off: radix heap";
                c.resolution_method = "default";
            }
            c.decide_ns = ns_since(t0);
        }

        // --- reference ------------------------------------------------------
        t0 = std::chrono::steady_clock::now();
        ReferenceDijkstraOptions ropts;
        ropts.target         = static_cast<NodeId>(o.target);
        const auto reference = reference_dijkstra_heap(g, static_cast<NodeId>(o.source), ropts);
        const std::uint64_t ref_ns = ns_since(t0);

        const auto hw = collect_hardware_info();

        Execution ex;
        if (o.pq == "binary_heap") ex = execute<BinaryHeap<Dist>>(c);
        else if (o.pq == "four_ary_heap") ex = execute<FourAryHeap<Dist>>(c);
        else if (o.pq == "pairing_heap") ex = execute<PairingHeap<Dist>>(c);
        else if (o.pq == "fibonacci_heap") ex = execute<FibonacciHeap<Dist>>(c);
        else if (o.pq == "radix_heap") ex = execute<RadixHeap<Dist>>(c);
        else if (o.pq == "lpq_optimized") {
            // Measure the queue the policy resolved to, not a stand-in: the
            // result below says which one that was.
            if (c.choice.kind == QueueKind::kRadixHeap) ex = execute<RadixHeap<Dist>>(c);
            else ex = execute<PooledBucketQueue<W>>(c);
        }
        else if (o.pq == "lpq_model") ex = execute<LpqStatic>(c);
        else if (o.pq == "lpq_online") ex = execute<LpqOnline>(c);
        else ex = execute<LpqPlain>(c);  // lpq and lpq_adaptive share a type

        std::ostringstream os;
        write_result(os, c, ex, reference, hw, load_ns, scale_ns, scorer_ns, ref_ns,
                     loaded.checksum);
        if (emit(result_out, os.str()) != 0) {
            std::cerr << "error: cannot write result\n";
            return 1;
        }
        return 0;
    } catch (const CategorisedError& e) {
        std::ostringstream os;
        write_failure(os, run_id, e.category, e.what());
        emit(result_out, os.str());
        std::cerr << "error: " << e.what() << "\n";
        return e.category == "invalid_request" ? 64 : 1;
    } catch (const std::exception& e) {
        std::ostringstream os;
        write_failure(os, run_id, "engine", e.what());
        emit(result_out, os.str());
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
