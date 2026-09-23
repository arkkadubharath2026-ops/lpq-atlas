// =============================================================================
// lpq_profile -- where does the learned priority queue's time go?
//
// A separate executable from lpq_benchmark, deliberately. The probes this
// binary compiles in perturb the code they measure, so putting them in the
// benchmark would contaminate every timing the project has already published.
// The division is: lpq_benchmark answers "how long", this answers "which part".
//
//   lpq_profile --dataset datasets/processed/dimacs_ny.lpqgraph
//               --queries-file experiments/manifests/dimacs_ny_queries_100.yaml
//               --pq lpq
//
// Reads no hardware performance counters (there are none on the development
// host; see docs/experiments/profiling.md) and reports no absolute durations,
// only shares of a total. See phase_profiler.hpp for why.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/common/error.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/graph/query_manifest.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/metrics/cpu_affinity.hpp"
#include "lpq/metrics/phase_profiler.hpp"
#include "lpq/pq/binary_heap.hpp"

// -----------------------------------------------------------------------------
// Allocation counting
// -----------------------------------------------------------------------------
//
// Replacing the global operator new is the only way to count every allocation
// the queue makes, including the ones inside std::vector that no probe in our
// own code can see. It is confined to this binary; the benchmark's allocator is
// untouched.
//
// The counter is switched on only around the measured region, so the graph
// load, the manifest parse and the iostreams setup are not counted as queue
// work. Unlike the timings, this number is exact.

// GCC sees `std::free` inside a replacement `operator delete` and, after
// inlining the pair into a std::vector teardown, reports a mismatched
// allocation function. It is a false positive by construction: these two
// functions ARE the allocation pair, and every pointer reaching the delete came
// from the malloc in the new above it. The suppression is scoped to these
// definitions rather than applied to the target, so a genuine mismatch
// anywhere else in this file is still an error.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    lpq::AllocationCounter::instance().note_allocation(size);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    lpq::AllocationCounter::instance().note_deallocation();
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    lpq::AllocationCounter::instance().note_deallocation();
    std::free(p);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace lpq {
namespace {

using W           = std::uint32_t;
using Dist        = DistanceType<W>;
using Graph       = CsrGraph<W>;
using ProfiledLpq = LearnedPriorityQueue<W, ConstantNodeScorer>;

struct Options {
    std::string dataset;
    std::string queries_file;
    std::string pq              = "lpq";
    std::size_t repetitions     = 3;
    int         core            = -1;
    bool        require_pinning = true;
};

[[noreturn]] void usage_and_exit(int code) {
    std::cout <<
        "lpq_profile -- phase-level profile of the priority queue\n\n"
        "  --dataset <file.lpqgraph>   converted graph (required)\n"
        "  --queries-file <file.yaml>  query manifest (required)\n"
        "  --pq lpq|binary_heap        queue to profile (default lpq)\n"
        "  --repetitions <n>           passes over the query set (default 3)\n"
        "  --core <n>                  logical core to pin to\n"
        "  --allow-unpinned            do not abort if pinning fails\n";
    std::exit(code);
}

[[nodiscard]] Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                throw InvalidConfigError(std::string(what) + " requires a value");
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage_and_exit(0);
        } else if (a == "--dataset") {
            o.dataset = next("--dataset");
        } else if (a == "--queries-file") {
            o.queries_file = next("--queries-file");
        } else if (a == "--pq") {
            o.pq = next("--pq");
        } else if (a == "--repetitions") {
            o.repetitions = static_cast<std::size_t>(std::stoul(next("--repetitions")));
        } else if (a == "--core") {
            o.core = std::stoi(next("--core"));
        } else if (a == "--allow-unpinned") {
            o.require_pinning = false;
        } else {
            throw InvalidConfigError("unknown argument: " + a);
        }
    }
    if (o.dataset.empty() || o.queries_file.empty()) {
        throw InvalidConfigError("--dataset and --queries-file are both required");
    }
    if (o.pq != "lpq" && o.pq != "binary_heap") {
        throw InvalidConfigError("--pq must be lpq or binary_heap");
    }
    if (o.repetitions == 0) {
        throw InvalidConfigError("--repetitions must be at least 1");
    }
    return o;
}

/// Mirrors the bucket width lpq_benchmark derives.
///
/// Duplicated rather than shared because the two runners have nothing else in
/// common, but the duplication matters: if this drifts, the profile would
/// describe a queue configured differently from the one that was benchmarked,
/// and its shares would not explain the timings they exist to explain.
[[nodiscard]] Dist derive_bucket_width(const GraphMetadata& meta) {
    const auto span = static_cast<std::uint64_t>(meta.max_weight > 0.0 ? meta.max_weight : 1.0);
    constexpr auto kBuckets = static_cast<std::uint64_t>(ProfiledLpq::Substrate::kBucketCount);
    std::uint64_t  width    = (span + kBuckets - 1U) / kBuckets;
    if (width == 0) {
        width = 1;
    }
    return static_cast<Dist>(width);
}

/// Measures what one probe pair costs, in the same ticks the phases are in.
///
/// This is not a detail. Six probe pairs sit inside every pop, so if a pair
/// costs a hundred ticks then a phase reported at a hundred ticks per call is
/// substantially instrument rather than queue. Rather than assume the probes
/// are cheap -- which is how a profile quietly lies -- the cost is measured
/// here and printed next to the phases so every per-call figure can be read
/// against it.
///
/// The loop body is empty, so what is timed is exactly one begin/end pair and
/// the accumulation behind it. The ticks are charged to `kCount`, which is
/// never reported, so the calibration cannot contaminate a real phase.
[[nodiscard]] double measure_probe_pair_cost(std::size_t iterations) {
#if LPQ_ENABLE_PHASE_PROFILING
    const std::uint64_t start = detail::read_ticks();
    for (std::size_t i = 0; i < iterations; ++i) {
        LPQ_PROFILE_BEGIN(calib);
        LPQ_PROFILE_END(calib, QueuePhase::kCount);
    }
    const std::uint64_t elapsed = detail::read_ticks() - start;
    PhaseProfile::instance().reset();
    return static_cast<double>(elapsed) / static_cast<double>(iterations);
#else
    (void)iterations;
    return 0.0;
#endif
}

template <typename Queue>
void run_all(const Graph& g, Queue& queue, const std::vector<NodeId>& sources,
             const std::vector<NodeId>& targets, std::size_t repetitions) {
    for (std::size_t r = 0; r < repetitions; ++r) {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            DijkstraOptions dopts;
            dopts.target       = targets[i];
            dopts.measure_time = false;
            const auto result  = dijkstra<W, Queue>(g, sources[i], queue, dopts);
            // Consumed so the search cannot be optimised away.
            if (result.dist.empty()) {
                std::cerr << "empty result\n";
            }
        }
    }
}

void print_report(const Options& o, const PhaseReport& rep, double probe_pair_ticks,
                  double measured_seconds) {
    std::printf("\n");
    std::printf("phase profile: %s\n", o.pq.c_str());
    std::printf("-------------------------------------------------------------------\n");
    std::printf("%-22s %14s %14s %10s %10s\n", "phase", "ticks", "calls", "%total", "%pop");

    for (const PhaseShare& s : rep.shares) {
        if (s.calls == 0 && s.ticks == 0) {
            continue;
        }
        const std::string name(phase_name(s.phase));
        const bool        nested =
            is_pop_subphase(s.phase) || s.phase == QueuePhase::kFallbackScan;
        std::printf("%s%-*s %14llu %14llu %9.2f%% ", nested ? "  " : "", nested ? 20 : 22,
                    name.c_str(), static_cast<unsigned long long>(s.ticks),
                    static_cast<unsigned long long>(s.calls), 100.0 * s.share_of_total);
        if (nested) {
            std::printf("%9.2f%%\n", 100.0 * s.share_of_pop);
        } else {
            std::printf("%10s\n", "-");
        }
    }
    std::printf("  %-20s %14llu %14s %10s %9.2f%%\n", "pop (unattributed)",
                static_cast<unsigned long long>(rep.pop_unattributed_ticks), "-", "-",
                100.0 * rep.pop_unattributed_share);
    std::printf("-------------------------------------------------------------------\n");
    std::printf("total ticks (push + pop) : %llu\n",
                static_cast<unsigned long long>(rep.total_ticks));
    std::printf("wall clock of region     : %.3f s\n", measured_seconds);
    std::printf("probe pairs              : %llu\n",
                static_cast<unsigned long long>(rep.probe_pairs));
    std::printf("cost of one probe pair   : %.1f ticks (measured on this run)\n",
                probe_pair_ticks);

    // Read against the line above: a phase whose per-call cost is comparable to
    // a probe pair has been measured at the noise floor of this instrument, and
    // its share says more about the probes than about the queue. Printing the
    // comparison rather than leaving it to the reader is the difference between
    // a profile and a misleading one.
    std::printf("\nper-call cost, against a probe pair at %.1f ticks\n", probe_pair_ticks);
    for (const PhaseShare& s : rep.shares) {
        if (s.calls == 0) {
            continue;
        }
        const double per_call = static_cast<double>(s.ticks) / static_cast<double>(s.calls);
        std::printf("  %-20s %10.1f ticks/call  %s\n",
                    std::string(phase_name(s.phase)).c_str(), per_call,
                    (probe_pair_ticks > 0.0 && per_call < 3.0 * probe_pair_ticks)
                        ? "<-- at or near the instrument floor"
                        : "");
    }
    std::printf("\n  Probe cost is INSIDE every figure above and is never subtracted.\n");

    std::printf("\nallocations in the measured region (exact, not timed)\n");
    std::printf("  allocations   : %llu\n", static_cast<unsigned long long>(rep.allocations));
    std::printf("  deallocations : %llu\n", static_cast<unsigned long long>(rep.deallocations));
    std::printf("  bytes         : %llu\n", static_cast<unsigned long long>(rep.allocated_bytes));
}

int run(int argc, char** argv) {
    const Options o = parse_args(argc, argv);

    // Same core selection as the benchmark harness, so the profile runs where
    // the timings it explains were taken.
    const int            core     = (o.core >= 0) ? o.core : default_benchmark_core();
    const AffinityResult affinity = pin_current_thread_to_core(core);
    if (!affinity.pinned) {
        std::cerr << "CPU pinning failed: " << affinity.detail << "\n";
        if (o.require_pinning) {
            std::cerr << "refusing to profile unpinned; pass --allow-unpinned to override\n";
            return 2;
        }
    } else {
        std::cout << "pinned to core " << affinity.core << "\n";
    }

    LoadedGraph<W>      loaded = read_binary_graph_file<W>(o.dataset);
    Graph               g      = std::move(loaded.graph);
    const GraphMetadata meta   = compute_metadata(g);

    const QueryManifest manifest = read_query_manifest_file(o.queries_file);
    manifest.validate_against(g);

    std::vector<NodeId> sources;
    std::vector<NodeId> targets;
    sources.reserve(manifest.queries.size());
    targets.reserve(manifest.queries.size());
    for (const ManifestQuery& q : manifest.queries) {
        sources.push_back(q.source);
        targets.push_back(q.target);
    }

    std::cout << "dataset      : " << std::filesystem::path(o.dataset).stem().string() << "\n"
              << "nodes / arcs : " << g.num_nodes() << " / " << g.num_arcs() << "\n"
              << "queries      : " << sources.size() << "\n"
              << "repetitions  : " << o.repetitions << "\n";

    // Calibrated on this run, on the pinned core, so the number describes this
    // machine in this thermal state rather than a remembered constant.
    const double probe_pair_ticks = measure_probe_pair_cost(2'000'000);

    // Wall clock of the measured region. Reported so the instrumented run can
    // be compared against the uninstrumented one built from the same source
    // (target lpq_profile_noinstr): the ratio IS the instrumentation overhead,
    // measured rather than assumed negligible.
    const auto region_start = std::chrono::steady_clock::now();

    if (o.pq == "lpq") {
        const CandidateWindowConfig window;
        ProfiledLpq                 queue{derive_bucket_width(meta), window};
        queue.reserve_nodes(g.num_nodes());

        // A warm-up pass outside the counters: the first query pays for the
        // queue's lazily grown storage, and attributing that to steady-state
        // phase shares would overstate allocation and bucket traversal.
        run_all(g, queue, sources, targets, 1);

        PhaseProfile::instance().reset();
        AllocationCounter::instance().reset();
        AllocationCounter::instance().set_enabled(true);
        run_all(g, queue, sources, targets, o.repetitions);
        AllocationCounter::instance().set_enabled(false);
    } else {
        BinaryHeap<Dist> queue;
        queue.reserve_nodes(g.num_nodes());
        run_all(g, queue, sources, targets, 1);

        PhaseProfile::instance().reset();
        AllocationCounter::instance().reset();
        AllocationCounter::instance().set_enabled(true);
        run_all(g, queue, sources, targets, o.repetitions);
        AllocationCounter::instance().set_enabled(false);
    }

    const std::chrono::duration<double> region_elapsed =
        std::chrono::steady_clock::now() - region_start;

    const PhaseReport rep = build_phase_report();
    print_report(o, rep, probe_pair_ticks, region_elapsed.count());

#if !LPQ_ENABLE_PHASE_PROFILING
    std::cerr << "\nWARNING: built without LPQ_ENABLE_PHASE_PROFILING; every figure "
                 "above is zero by construction.\n";
#endif
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
