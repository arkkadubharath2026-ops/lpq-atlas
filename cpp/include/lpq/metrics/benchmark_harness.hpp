// =============================================================================
// Benchmark measurement protocol (PRD.md section 18, ESD.md section 21).
//
// PRD.md section 18 requires: the same graph, the same source-target pairs, the
// same compiler settings, the same hardware, the same thread policy, multiple
// repetitions, warm-up separated from measured runs, and no cherry-picking.
// This harness is where those requirements are implemented once, so that every
// algorithm is measured identically and a difference in results cannot come
// from a difference in how they were timed.
//
// THE PROTOCOL
//
//   1. Pin to a single core, before anything is timed. Mandatory on this
//      hybrid-core host; see cpu_affinity.hpp. If pinning fails the run
//      continues but every record says so, because unpinned numbers carry
//      migration noise and must not be mistaken for pinned ones.
//
//   2. Warm-up repetitions, discarded. These populate caches, fault in pages,
//      let the branch predictor settle and let the CPU reach a steady clock.
//      Their timings are recorded separately rather than thrown away, so that
//      "the first run was 40x slower" is visible evidence rather than a silent
//      correction.
//
//   3. Measured repetitions, all retained. Every per-repetition timing is kept
//      in the result record (PRD.md section 22 forbids silently discarding
//      outliers), and the summary is median + MAD, which is robust to the ones
//      that occur anyway.
//
// WHAT IS AND IS NOT TIMED
//
// The timed region covers the shortest-path search only. Queue allocation,
// graph loading and result-array allocation sit outside it, because a queue is
// allocated once per graph in real use and timing it per query would make the
// measurement largely an allocator benchmark (PRD.md section 24). The result
// arrays are reused across repetitions for the same reason.
// =============================================================================
#ifndef LPQ_METRICS_BENCHMARK_HARNESS_HPP
#define LPQ_METRICS_BENCHMARK_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lpq/common/clock.hpp"
#include "lpq/common/error.hpp"
#include "lpq/metrics/cpu_affinity.hpp"
#include "lpq/metrics/statistics.hpp"

namespace lpq {

/// Measurement configuration.
struct BenchmarkConfig {
    /// Discarded repetitions run before measurement begins.
    std::size_t warmup_repetitions = 3;

    /// Measured repetitions. The default of 11 is odd so the median is an
    /// actual observation rather than an interpolation between two, and is
    /// large enough for the MAD to be meaningful while keeping a full
    /// experiment sweep tractable.
    std::size_t repetitions = 11;

    /// Logical core to pin to; -1 selects the default (see
    /// default_benchmark_core()).
    int cpu_core = -1;

    /// Abort rather than continue when pinning fails.
    ///
    /// Off by default so exploratory runs work in restricted containers, but
    /// publication-tier experiment manifests set it, because an unpinned
    /// measurement on this hardware is not worth publishing.
    bool require_pinning = false;

    /// Raise thread priority for the duration of the run.
    bool raise_priority = true;

    /// Relative MAD above which the run is flagged as noisy. 0.05 means the
    /// median absolute deviation exceeded 5% of the median, which on a pinned
    /// core indicates interference rather than genuine variance.
    double noise_threshold = 0.05;
};

/// Outcome of a measured benchmark.
struct BenchmarkMeasurement {
    /// Every measured repetition, in order. Retained so any figure can be
    /// traced back to actual measurements.
    std::vector<std::uint64_t> samples_ns;
    /// Warm-up timings, kept separate. Never mixed into the summary.
    std::vector<std::uint64_t> warmup_ns;

    /// Robust summary: median and MAD are the headline figures.
    SampleSummary summary;

    AffinityResult affinity;
    bool           priority_raised = false;

    /// True when relative MAD exceeded the configured threshold.
    bool noisy = false;

    /// Human-readable warnings to surface in reports. Empty when the
    /// measurement met every quality condition.
    std::vector<std::string> warnings;

    /// The headline number for comparisons.
    [[nodiscard]] double median_ns() const noexcept { return summary.median; }
    [[nodiscard]] double mad_ns() const noexcept { return summary.mad; }

    /// True when this measurement is fit to support a published claim.
    [[nodiscard]] bool publication_quality() const noexcept {
        return affinity.pinned && !noisy && summary.count >= 5U;
    }
};

/// Runs `body` under the measurement protocol.
///
/// `body` performs one complete unit of work -- typically one shortest-path
/// query -- and is called once per repetition. It must be self-contained and
/// leave no state that would make a later repetition faster or slower, or the
/// repetitions are not independent samples.
///
/// The return value of `body` is passed to `consume` so the optimizer cannot
/// delete the work as dead. Timing an empty loop is the classic way a
/// microbenchmark reports an impossible speedup.
template <typename Body>
[[nodiscard]] BenchmarkMeasurement run_benchmark(Body&& body, const BenchmarkConfig& cfg = {}) {
    BenchmarkMeasurement m;

    if (cfg.repetitions == 0) {
        throw InvalidConfigError("benchmark repetitions must be at least 1");
    }

    // --- 1. Pin before anything is timed ------------------------------------
    const int core = (cfg.cpu_core >= 0) ? cfg.cpu_core : default_benchmark_core();
    m.affinity     = pin_current_thread_to_core(core);
    if (!m.affinity.pinned) {
        const std::string w = "CPU pinning failed: " + m.affinity.detail +
                              ". Timings carry scheduler-migration noise and are not "
                              "comparable with pinned runs.";
        if (cfg.require_pinning) {
            throw InvalidConfigError(w);
        }
        m.warnings.push_back(w);
    }

    if (cfg.raise_priority) {
        m.priority_raised = raise_benchmark_thread_priority();
    }

    // --- 2. Warm-up, recorded but never summarised --------------------------
    m.warmup_ns.reserve(cfg.warmup_repetitions);
    for (std::size_t i = 0; i < cfg.warmup_repetitions; ++i) {
        const TimePoint t0 = now();
        body();
        m.warmup_ns.push_back(elapsed_ns_since(t0));
    }

    // --- 3. Measured repetitions, all retained ------------------------------
    m.samples_ns.reserve(cfg.repetitions);
    for (std::size_t i = 0; i < cfg.repetitions; ++i) {
        const TimePoint t0 = now();
        body();
        m.samples_ns.push_back(elapsed_ns_since(t0));
    }

    m.summary = summarise_ns(m.samples_ns);

    // --- Quality checks -----------------------------------------------------
    if (m.summary.relative_mad() > cfg.noise_threshold) {
        m.noisy = true;
        m.warnings.push_back(
            "measurement is noisy: relative MAD " +
            std::to_string(m.summary.relative_mad() * 100.0) + "% exceeds the " +
            std::to_string(cfg.noise_threshold * 100.0) + "% threshold");
    }

    if (m.summary.mean_median_diverges()) {
        m.warnings.push_back(
            "mean and median differ by more than 10%, indicating outliers or skew; "
            "the median is reported and no sample has been discarded");
    }

    // A measurement near the clock's floor is not a measurement. The floor here
    // is the *measured* resolution, not the nominal period: on Windows the
    // period advertises 1 ns while the counter advances in 100 ns steps, and
    // trusting the nominal value made this check inert on exactly the platform
    // that needed it.
    //
    // A median of zero is the most extreme case, not an exempt one. An earlier
    // version guarded with `median > 0.0` and so stayed silent precisely when
    // the work was entirely below the clock floor -- the situation most likely
    // to be mistaken for an impossibly fast algorithm.
    const double resolution = effective_clock_resolution_ns();
    if (m.summary.median <= 0.0) {
        m.noisy = true;
        m.warnings.push_back(
            "median duration is zero: every repetition completed within one tick of "
            "the clock (" + std::to_string(resolution) +
            " ns). This is not a measurement. Batch more work per repetition.");
    } else if (m.summary.median < resolution * 100.0) {
        m.warnings.push_back(
            "median duration " + std::to_string(m.summary.median) +
            " ns is within 100x of the measured clock resolution (" +
            std::to_string(resolution) +
            " ns), so it is quantised rather than precise; batch more work per "
            "repetition for a trustworthy timing");
    }

    return m;
}

/// Prevents the optimizer from discarding a computed value.
///
/// An empty asm block with the value as an input constrains the compiler to
/// materialise it, without emitting any instruction. The fallback for MSVC
/// writes through a volatile pointer, which is heavier but equally effective.
template <typename T>
inline void do_not_optimize(T const& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    volatile const void* sink = &value;
    (void)sink;
    _ReadWriteBarrier();
#endif
}

}  // namespace lpq

#endif  // LPQ_METRICS_BENCHMARK_HARNESS_HPP
