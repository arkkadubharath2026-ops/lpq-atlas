// =============================================================================
// Result record schema (ESD.md section 23).
//
// ESD.md section 23 lists the fields every result record must carry. This is
// that schema, plus the robust statistics of PRD.md section 22 and the raw
// per-repetition samples that make a figure traceable to measurements.
//
// TWO ENCODING DECISIONS WORTH STATING
//
//   * Unmeasured quantities are JSON null, never zero. The hardware counters
//     are absent on this host, and "cache_misses: 0" would be indistinguishable
//     from a real measurement of zero. Null is the only honest encoding, and
//     the analysis scripts treat it as missing rather than as a value.
//
//   * Optional measurements carry an explicit `has_*` flag rather than relying
//     on a sentinel value. Sentinels get compared against by accident;
//     a flag does not.
//
// The schema version is embedded in every record. Raw results are immutable
// (ESD.md section 31), so results written under an older schema must remain
// readable, and an analysis script needs to know which shape it is looking at.
// =============================================================================
#ifndef LPQ_METRICS_RESULT_RECORD_HPP
#define LPQ_METRICS_RESULT_RECORD_HPP

#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "lpq/algorithms/shortest_path_result.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/metrics/hardware_info.hpp"
#include "lpq/metrics/json_writer.hpp"
#include "lpq/metrics/statistics.hpp"

namespace lpq {

/// Incremented only on a breaking change to the record shape.
inline constexpr int kResultSchemaVersion = 1;

/// Machine-level counters (PRD.md section 20).
///
/// Every field is optional. On a host with no PMU they are all empty and
/// serialise as null.
struct HardwareCounters {
    std::optional<std::uint64_t> cycles;
    std::optional<std::uint64_t> instructions;
    std::optional<std::uint64_t> branches;
    std::optional<std::uint64_t> branch_misses;
    std::optional<std::uint64_t> cache_references;
    std::optional<std::uint64_t> cache_misses;
    std::optional<std::uint64_t> llc_misses;
    std::optional<std::uint64_t> peak_rss_bytes;

    /// Why counters are absent, for the record. Empty when they were collected.
    std::string unavailable_reason;

    [[nodiscard]] bool any_collected() const noexcept {
        return cycles || instructions || branches || branch_misses || cache_references ||
               cache_misses || llc_misses;
    }
};

/// ML timing breakdown (PRD.md section 20). Zero for classical queues; the
/// fields exist from the start so the schema does not change when LPQ lands.
struct MlTimings {
    std::uint64_t feature_extraction_ns = 0;
    std::uint64_t inference_ns          = 0;
    std::uint64_t verification_ns       = 0;
    std::uint64_t online_update_ns      = 0;

    std::uint64_t predictions           = 0;
    std::uint64_t correct_predictions   = 0;
    std::uint64_t candidate_checks      = 0;
    std::uint64_t corrections           = 0;
    std::uint64_t fallbacks             = 0;

    [[nodiscard]] std::uint64_t total_ns() const noexcept {
        return feature_extraction_ns + inference_ns + verification_ns + online_update_ns;
    }
};

/// One benchmarked query.
struct ResultRecord {
    // --- Identity (ESD.md section 23) ---------------------------------------
    std::string experiment_id;
    std::string timestamp;  ///< ISO-8601 UTC, wall clock; never used for timing.
    std::string dataset;
    std::string algorithm;  ///< "dijkstra", "astar"
    std::string pq;         ///< "binary_heap", "radix_heap", "lpq", ...
    std::string mode;       ///< "exact", "approximate", "adaptive"

    std::size_t graph_nodes = 0;
    std::size_t graph_edges = 0;

    NodeId source = kInvalidNode;
    NodeId target = kInvalidNode;

    // --- Timing --------------------------------------------------------------
    /// Median is the headline figure; raw samples are retained.
    BenchmarkMeasurement timing;

    // --- Queue operations (PRD.md section 20) --------------------------------
    SearchCounters counters;

    // --- ML ------------------------------------------------------------------
    MlTimings ml;

    // --- Hardware ------------------------------------------------------------
    HardwareCounters hw_counters;
    HardwareInfo     hardware;

    // --- Correctness (PRD.md section 16) -------------------------------------
    /// Cost returned by the implementation under test.
    std::optional<double> path_cost;
    /// Cost from the trusted reference. Absent only when validation was
    /// deliberately skipped, which the validator reports.
    std::optional<double> optimal_cost;
    /// path_cost / optimal_cost. No accuracy result may be reported without the
    /// reference, so this is present only when both costs are.
    std::optional<double> approximation_ratio;
    /// Whether the returned result matched the reference exactly.
    std::optional<bool> exact_match;

    bool target_reachable = false;
    std::string termination;

    /// The queue configuration this record was measured under. Optional so that
    /// records from before this existed still serialise identically. Without it a
    /// bucket-width sweep leaves records that cannot be told apart.
    struct QueueConfig {
        std::string   label;             ///< the name the row is reported under
        std::string   queue;             ///< the queue implementation
        std::uint64_t bucket_width = 0;  ///< 0 for queues with no buckets
        std::uint64_t ring_size    = 0;
        std::int64_t  front        = -1; ///< -1: not applicable
        std::string   strategy;          ///< "modular" | "direct" | ""
        std::uint32_t position     = 0;  ///< place in this query's measurement order

        /// For a queue that resolves to another one (lpq_optimized): which queue
        /// it actually ran, why, and what deciding cost. Empty otherwise. A row
        /// whose resolved queue is a classical heap is that heap's result and
        /// must be read as such.
        std::string   resolved_queue;
        std::string   resolution_reason;
        double        predicted_ratio = 0.0;     ///< vs radix heap; 0 if not predicted
        std::uint64_t profile_ns      = 0;       ///< weight-profile computation, once per graph
        std::string   policy_source;
        std::string   profile_source;            ///< computed | computed_and_cached | cached
        std::string   resolution_method;         ///< autotune | autotune_cached | policy | default
        std::uint64_t calibration_ns = 0;        ///< autotune measurement cost (0 when cached)
    };
    std::optional<QueueConfig> queue_config;

    /// Set when the configuration could not run at all -- for example a radix
    /// heap requested for floating-point weights. A record is still emitted, so
    /// a missing row in a results table is explained rather than absent.
    std::string unsupported_reason;

    [[nodiscard]] bool supported() const noexcept { return unsupported_reason.empty(); }
};

/// Serialises one record.
inline void write_record(JsonWriter& w, const ResultRecord& r) {
    w.begin_object();

    w.key_value("schema_version", static_cast<std::int64_t>(kResultSchemaVersion));
    w.key_value("experiment_id", r.experiment_id);
    w.key_value("timestamp", r.timestamp);
    w.key_value("dataset", r.dataset);
    w.key_value("algorithm", r.algorithm);
    w.key_value("pq", r.pq);
    w.key_value("mode", r.mode);
    w.key_value("graph_nodes", r.graph_nodes);
    w.key_value("graph_edges", r.graph_edges);
    w.key_value("source", static_cast<std::uint64_t>(r.source));

    if (r.target == kInvalidNode) {
        w.key_null("target");
    } else {
        w.key_value("target", static_cast<std::uint64_t>(r.target));
    }

    w.key_value("supported", r.supported());
    if (!r.supported()) {
        w.key_value("unsupported_reason", r.unsupported_reason);
    }
    if (r.queue_config) {
        const auto& qc = *r.queue_config;
        w.begin_object("queue_config");
        w.key_value("label", qc.label);
        w.key_value("queue", qc.queue);
        w.key_value("bucket_width", qc.bucket_width);
        w.key_value("ring_size", qc.ring_size);
        w.key_value("front", qc.front);
        w.key_value("strategy", qc.strategy);
        w.key_value("order_position", static_cast<std::uint64_t>(qc.position));
        if (!qc.resolved_queue.empty()) {
            w.key_value("resolved_queue", qc.resolved_queue);
            w.key_value("resolution_reason", qc.resolution_reason);
            w.key_value("predicted_ratio_vs_radix", qc.predicted_ratio);
            w.key_value("profile_ns", qc.profile_ns);
            w.key_value("policy_source", qc.policy_source);
            w.key_value("profile_source", qc.profile_source);
            w.key_value("resolution_method", qc.resolution_method);
            w.key_value("calibration_ns", qc.calibration_ns);
        }
        w.end_object();
    }

    // --- Timing --------------------------------------------------------------
    w.begin_object("timing");
    {
        const auto& s = r.timing.summary;
        // Median and MAD first: these are the figures comparisons are made on.
        w.key_value("median_ns", s.median);
        w.key_value("mad_ns", s.mad);
        w.key_value("mad_scaled_ns", s.mad_scaled);
        w.key_value("relative_mad", s.relative_mad());
        // Mean and stddev retained for comparison, not for decisions.
        w.key_value("mean_ns", s.mean);
        w.key_value("stddev_ns", s.stddev);
        w.key_value("min_ns", s.min);
        w.key_value("max_ns", s.max);
        w.key_value("p25_ns", s.p25);
        w.key_value("p75_ns", s.p75);
        w.key_value("p95_ns", s.p95);
        w.key_value("p99_ns", s.p99);
        w.key_value("repetitions", s.count);
        // Raw samples: PRD.md section 22 requires distributions, not only
        // summaries, and forbids discarding outliers.
        w.key_array("samples_ns", r.timing.samples_ns);
        w.key_array("warmup_ns", r.timing.warmup_ns);
        w.key_value("noisy", r.timing.noisy);
        w.key_value("publication_quality", r.timing.publication_quality());
        w.key_value("cpu_pinned", r.timing.affinity.pinned);
        w.key_value("cpu_core", r.timing.affinity.core);
        if (!r.timing.affinity.pinned) {
            w.key_value("cpu_pinning_failure", r.timing.affinity.detail);
        }
        w.begin_array("warnings");
        for (const auto& warning : r.timing.warnings) {
            w.value(warning);
        }
        w.end_array();
    }
    w.end_object();

    // --- Queue operations ----------------------------------------------------
    w.begin_object("pq_operations");
    w.key_value("pushes", r.counters.pq_pushes);
    w.key_value("pops", r.counters.pq_pops);
    w.key_value("decrease_keys", r.counters.pq_decrease_keys);
    w.key_value("comparisons", r.counters.pq_comparisons);
    w.key_value("stale_pops", r.counters.stale_pops);
    w.key_value("settled_nodes", r.counters.settled_nodes);
    w.key_value("relaxations", r.counters.relaxations);
    w.key_value("successful_relaxations", r.counters.successful_relaxations);
    w.key_value("peak_queue_size", r.counters.peak_queue_size);
    w.end_object();

    // --- ML ------------------------------------------------------------------
    w.begin_object("ml");
    w.key_value("feature_extraction_ns", r.ml.feature_extraction_ns);
    w.key_value("inference_ns", r.ml.inference_ns);
    w.key_value("verification_ns", r.ml.verification_ns);
    w.key_value("online_update_ns", r.ml.online_update_ns);
    w.key_value("total_overhead_ns", r.ml.total_ns());
    w.key_value("predictions", r.ml.predictions);
    w.key_value("correct_predictions", r.ml.correct_predictions);
    w.key_value("candidate_checks", r.ml.candidate_checks);
    w.key_value("corrections", r.ml.corrections);
    w.key_value("fallbacks", r.ml.fallbacks);
    w.end_object();

    // --- Hardware counters ---------------------------------------------------
    w.begin_object("hardware_counters");
    {
        // null, never zero: a reader must be able to distinguish "not measured"
        // from "measured as zero".
        auto opt = [&w](std::string_view k, const std::optional<std::uint64_t>& v) {
            if (v) {
                w.key_value(k, *v);
            } else {
                w.key_null(k);
            }
        };
        opt("cycles", r.hw_counters.cycles);
        opt("instructions", r.hw_counters.instructions);
        opt("branches", r.hw_counters.branches);
        opt("branch_misses", r.hw_counters.branch_misses);
        opt("cache_references", r.hw_counters.cache_references);
        opt("cache_misses", r.hw_counters.cache_misses);
        opt("llc_misses", r.hw_counters.llc_misses);
        opt("peak_rss_bytes", r.hw_counters.peak_rss_bytes);
        w.key_value("collected", r.hw_counters.any_collected());
        if (!r.hw_counters.unavailable_reason.empty()) {
            w.key_value("unavailable_reason", r.hw_counters.unavailable_reason);
        }
    }
    w.end_object();

    // --- Correctness ---------------------------------------------------------
    w.begin_object("correctness");
    if (r.path_cost) {
        w.key_value("path_cost", *r.path_cost);
    } else {
        w.key_null("path_cost");
    }
    if (r.optimal_cost) {
        w.key_value("optimal_cost", *r.optimal_cost);
    } else {
        w.key_null("optimal_cost");
    }
    if (r.approximation_ratio) {
        w.key_value("approximation_ratio", *r.approximation_ratio);
    } else {
        w.key_null("approximation_ratio");
    }
    if (r.exact_match) {
        w.key_value("exact_match", *r.exact_match);
    } else {
        w.key_null("exact_match");
    }
    w.key_value("target_reachable", r.target_reachable);
    w.key_value("termination", r.termination);
    w.end_object();

    // --- Environment ---------------------------------------------------------
    w.begin_object("environment");
    w.key_value("cpu_model", r.hardware.cpu_model);
    w.key_value("logical_cores", static_cast<std::uint64_t>(r.hardware.logical_cores));
    w.key_value("total_ram_bytes", r.hardware.total_ram_bytes);
    w.key_value("os", r.hardware.os);
    w.key_value("compiler", r.hardware.compiler);
    w.key_value("compiler_version", r.hardware.compiler_version);
    w.key_value("build_type", r.hardware.build_type);
    w.key_value("optimization_flags", r.hardware.optimization_flags);
    w.key_value("instrumentation_enabled", r.hardware.instrumentation_enabled);
    w.key_value("native_arch", r.hardware.native_arch);
    w.key_value("clock_resolution_ns", r.hardware.clock_resolution_ns);
    w.key_value("git_commit", r.hardware.git_commit);
    w.key_value("git_dirty", r.hardware.git_dirty);
    w.key_value("hardware_counters_available", r.hardware.hardware_counters_available);
    w.end_object();

    w.end_object();
}

/// ISO-8601 UTC timestamp for a record.
///
/// Wall-clock, from system_clock, and used only for labelling. Every duration
/// in the record comes from the monotonic steady_clock instead, so an NTP step
/// mid-run cannot corrupt a measurement.
[[nodiscard]] inline std::string iso8601_now() {
    const auto  now_tp = std::chrono::system_clock::now();
    const auto  t      = std::chrono::system_clock::to_time_t(now_tp);
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

}  // namespace lpq

#endif  // LPQ_METRICS_RESULT_RECORD_HPP
