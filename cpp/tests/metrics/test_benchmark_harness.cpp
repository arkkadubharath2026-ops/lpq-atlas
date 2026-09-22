// Tests for the benchmark measurement protocol (PRD.md section 18).
//
// These verify the protocol's structure -- pinning attempted, warm-up separated
// from measurement, every sample retained, robust summary computed -- rather
// than asserting on wall-clock durations, which would be flaky by construction
// on a shared machine.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/metrics/cpu_affinity.hpp"

namespace {

using lpq::BenchmarkConfig;
using lpq::do_not_optimize;
using lpq::run_benchmark;

/// Work the optimizer cannot remove or evaluate in closed form.
///
/// An earlier version accumulated `i * 2654435761U`, which is an arithmetic
/// series: GCC computed the whole loop at compile time and deleted it, so every
/// measurement came back as exactly zero. That is precisely the failure
/// DoNotOptimizePreventsDeadCodeElimination exists to detect, and it did.
///
/// Two changes make the loop survive: the accumulator is fed back into itself
/// through a multiply and xor-shift, so iteration n+1 genuinely depends on
/// iteration n and no closed form exists; and do_not_optimize is applied inside
/// the loop, which forbids hoisting or vectorising it away.
std::uint64_t busy(std::uint64_t iterations) {
    std::uint64_t acc = 0x9E3779B97F4A7C15ULL;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        acc += i;
        acc *= 0xBF58476D1CE4E5B9ULL;
        acc ^= acc >> 31;
        do_not_optimize(acc);
    }
    do_not_optimize(acc);
    return acc;
}

TEST(BenchmarkHarness, RunsTheConfiguredRepetitionCounts) {
    BenchmarkConfig cfg;
    cfg.warmup_repetitions = 2;
    cfg.repetitions        = 7;

    int calls = 0;
    const auto m = run_benchmark([&] { ++calls; do_not_optimize(calls); }, cfg);

    EXPECT_EQ(calls, 9) << "2 warm-up plus 7 measured";
    EXPECT_EQ(m.warmup_ns.size(), 2U);
    EXPECT_EQ(m.samples_ns.size(), 7U);
    EXPECT_EQ(m.summary.count, 7U);
}

TEST(BenchmarkHarness, WarmupIsExcludedFromTheSummary) {
    // The summary must describe the measured repetitions only. Mixing warm-up
    // in would bias every reported median upward by the cold-start cost.
    BenchmarkConfig cfg;
    cfg.warmup_repetitions = 5;
    cfg.repetitions        = 5;

    const auto m = run_benchmark([] { do_not_optimize(busy(1000)); }, cfg);
    EXPECT_EQ(m.summary.count, m.samples_ns.size());
    EXPECT_NE(m.summary.count, m.samples_ns.size() + m.warmup_ns.size());
}

TEST(BenchmarkHarness, WarmupTimingsAreRetainedNotDiscarded) {
    // Kept so that "the first run was far slower" is visible evidence rather
    // than a silent correction.
    BenchmarkConfig cfg;
    cfg.warmup_repetitions = 3;
    cfg.repetitions        = 3;

    const auto m = run_benchmark([] { do_not_optimize(busy(1000)); }, cfg);
    EXPECT_EQ(m.warmup_ns.size(), 3U);
}

TEST(BenchmarkHarness, ZeroWarmupIsAllowed) {
    BenchmarkConfig cfg;
    cfg.warmup_repetitions = 0;
    cfg.repetitions        = 3;

    const auto m = run_benchmark([] { do_not_optimize(busy(100)); }, cfg);
    EXPECT_TRUE(m.warmup_ns.empty());
    EXPECT_EQ(m.samples_ns.size(), 3U);
}

TEST(BenchmarkHarness, RejectsZeroRepetitions) {
    BenchmarkConfig cfg;
    cfg.repetitions = 0;
    EXPECT_THROW((void)run_benchmark([] {}, cfg), lpq::InvalidConfigError);
}

TEST(BenchmarkHarness, AttemptsCpuPinning) {
    BenchmarkConfig cfg;
    cfg.repetitions = 3;

    const auto m = run_benchmark([] { do_not_optimize(busy(100)); }, cfg);

    // Pinning can legitimately fail in a restricted container, so the value is
    // not asserted. What must hold is that the attempt is recorded either way,
    // so an unpinned run is never mistaken for a pinned one.
    if (m.affinity.pinned) {
        EXPECT_GE(m.affinity.core, 0);
    } else {
        EXPECT_FALSE(m.affinity.detail.empty())
            << "a failed pinning must explain itself";
        EXPECT_FALSE(m.warnings.empty())
            << "an unpinned run must carry a warning in its record";
    }
}

TEST(BenchmarkHarness, RequirePinningTurnsFailureIntoAnError) {
    // Publication-tier manifests set this: an unpinned measurement on hybrid
    // hardware is not worth publishing.
    BenchmarkConfig cfg;
    cfg.repetitions     = 2;
    cfg.require_pinning = true;
    cfg.cpu_core        = 9999;  // certainly out of range

    EXPECT_THROW((void)run_benchmark([] {}, cfg), lpq::InvalidConfigError);
}

TEST(BenchmarkHarness, OutOfRangeCoreIsReportedNotSilentlyIgnored) {
    BenchmarkConfig cfg;
    cfg.repetitions = 2;
    cfg.cpu_core    = 9999;

    const auto m = run_benchmark([] {}, cfg);
    EXPECT_FALSE(m.affinity.pinned);
    EXPECT_NE(m.affinity.detail.find("outside the range"), std::string::npos);
}

TEST(BenchmarkHarness, ComputesARobustSummary) {
    BenchmarkConfig cfg;
    cfg.repetitions = 11;

    const auto m = run_benchmark([] { do_not_optimize(busy(200000)); }, cfg);

    EXPECT_GT(m.median_ns(), 0.0);
    EXPECT_GE(m.mad_ns(), 0.0);
    EXPECT_LE(m.summary.min, m.summary.median);
    EXPECT_LE(m.summary.median, m.summary.max);
}

TEST(BenchmarkHarness, PublicationQualityRequiresPinningAndLowNoise) {
    BenchmarkConfig cfg;
    cfg.repetitions = 11;

    const auto m = run_benchmark([] { do_not_optimize(busy(200000)); }, cfg);

    // The flag must agree with its own stated conditions, whatever this
    // particular machine happened to do during the run.
    const bool expected = m.affinity.pinned && !m.noisy && m.summary.count >= 5U;
    EXPECT_EQ(m.publication_quality(), expected);
}

TEST(BenchmarkHarness, TooFewRepetitionsIsNotPublicationQuality) {
    BenchmarkConfig cfg;
    cfg.repetitions        = 3;
    cfg.warmup_repetitions = 0;

    const auto m = run_benchmark([] { do_not_optimize(busy(1000)); }, cfg);
    EXPECT_FALSE(m.publication_quality())
        << "three repetitions cannot support a published claim";
}

TEST(BenchmarkHarness, WarnsWhenTimingsApproachClockResolution) {
    // A measurement near the clock's tick is quantised, not measured. On
    // Windows the steady clock ticks at 100 ns, so an empty body must trip this.
    BenchmarkConfig cfg;
    cfg.repetitions        = 11;
    cfg.warmup_repetitions = 1;

    const auto m = run_benchmark([] {}, cfg);

    const bool warned_about_resolution =
        std::any_of(m.warnings.begin(), m.warnings.end(), [](const std::string& w) {
            return w.find("clock") != std::string::npos;
        });
    // An empty body cannot take longer than a clock tick, so the warning is
    // mandatory here rather than conditional.
    EXPECT_LT(m.summary.median, lpq::effective_clock_resolution_ns() * 100.0)
        << "test premise: an empty body must be sub-resolution";
    EXPECT_TRUE(warned_about_resolution)
        << "a sub-resolution timing must be flagged, not reported as precise";
}

TEST(BenchmarkHarness, AZeroMedianIsFlaggedRatherThanSilentlyAccepted) {
    // The most extreme sub-resolution case, and the one an earlier version of
    // the guard skipped: a median of exactly zero produced no warning at all.
    BenchmarkConfig cfg;
    cfg.repetitions        = 11;
    cfg.warmup_repetitions = 0;

    const auto m = run_benchmark([] {}, cfg);
    if (m.summary.median == 0.0) {
        EXPECT_TRUE(m.noisy) << "a zero median is not a measurement";
        EXPECT_FALSE(m.publication_quality());
        const bool said_so =
            std::any_of(m.warnings.begin(), m.warnings.end(), [](const std::string& w) {
                return w.find("not a measurement") != std::string::npos;
            });
        EXPECT_TRUE(said_so);
    }
}

TEST(Clock, EffectiveResolutionIsMeasuredNotAssumed) {
    // The nominal period advertises 1 ns on Windows while the counter actually
    // advances in 100 ns steps. Trusting the nominal value made the harness's
    // resolution warning inert on that platform.
    const double effective = lpq::effective_clock_resolution_ns();
    const double nominal   = lpq::nominal_clock_resolution_ns();

    EXPECT_GT(effective, 0.0);
    EXPECT_GE(effective, nominal)
        << "a clock cannot resolve finer than its own representation";

    // Cached: repeated calls must not re-probe, and must agree.
    EXPECT_DOUBLE_EQ(effective, lpq::effective_clock_resolution_ns());
}

TEST(BenchmarkHarness, DoNotOptimizePreventsDeadCodeElimination) {
    // If the optimizer removed the loop, the measured time would collapse to
    // the clock resolution and the benchmark would report an impossible speedup.
    BenchmarkConfig cfg;
    cfg.repetitions        = 5;
    cfg.warmup_repetitions = 1;

    const auto small = run_benchmark([] { do_not_optimize(busy(20000)); }, cfg);
    const auto large = run_benchmark([] { do_not_optimize(busy(2000000)); }, cfg);

    EXPECT_GT(large.median_ns(), small.median_ns())
        << "200x more work must take measurably longer; if not, the work was "
           "optimized away and every timing in this project is meaningless";
}

// --- Affinity -----------------------------------------------------------------

TEST(CpuAffinity, ReportsLogicalProcessorCount) {
    EXPECT_GT(lpq::logical_processor_count(), 0U);
}

TEST(CpuAffinity, DefaultCoreAvoidsCoreZero) {
    // Core 0 attracts a disproportionate share of interrupt and kernel work on
    // both Windows and Linux.
    if (lpq::logical_processor_count() > 1U) {
        EXPECT_NE(lpq::default_benchmark_core(), 0);
    }
}

TEST(CpuAffinity, DefaultCoreIsInRange) {
    const int  core = lpq::default_benchmark_core();
    const auto n    = static_cast<int>(lpq::logical_processor_count());
    EXPECT_GE(core, 0);
    EXPECT_LT(core, n);
}

TEST(CpuAffinity, RejectsNegativeAndOutOfRangeCores) {
    EXPECT_FALSE(lpq::pin_current_thread_to_core(-1).pinned);
    EXPECT_FALSE(lpq::pin_current_thread_to_core(100000).pinned);
}

TEST(CpuAffinity, DescribeIsHumanReadable) {
    const auto r = lpq::pin_current_thread_to_core(lpq::default_benchmark_core());
    EXPECT_FALSE(r.describe().empty());
    if (!r.pinned) {
        EXPECT_NE(r.describe().find("NOT PINNED"), std::string::npos);
    }
}

}  // namespace
