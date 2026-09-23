// =============================================================================
// Phase-level profiling for the priority queue (PRD.md section 20).
//
// This exists to answer one question that the comparison counters cannot: the
// learned queue performs FEWER comparisons than the binary heap on a road
// network and is several times slower, so the time is going somewhere the
// comparison count does not see. This header measures where, by attributing
// elapsed time to disjoint phases of queue work.
//
// -----------------------------------------------------------------------------
// What this is not
// -----------------------------------------------------------------------------
//
// It is NOT a hardware profiler. It reads no performance counters, and it
// cannot say anything about cache misses, branch mispredictions or memory
// bandwidth -- the development host runs under WSL2, which exposes no CPU PMU
// (docs/experiments/profiling.md). Any claim about cache behaviour remains a
// hypothesis unless it is measured with a tool that can actually measure it.
//
// What this CAN establish is the share of elapsed time spent in each phase,
// which is enough to identify a dominant overhead without needing to know why
// that phase is expensive.
//
// -----------------------------------------------------------------------------
// Why ticks rather than nanoseconds
// -----------------------------------------------------------------------------
//
// The profiler accumulates raw timestamp-counter ticks and reports SHARES of a
// total, never absolute durations. That is deliberate: converting ticks to
// nanoseconds needs a calibrated frequency, and on a laptop whose clock scales
// with load and temperature that calibration is exactly the sort of derived
// number this project refuses to report as if it were measured. A ratio of two
// tick counts taken on the same core needs no calibration at all.
//
// The cost is that this header cannot report "the verification scan takes
// N nanoseconds". It reports "the verification scan is X% of queue time", which
// is the question being asked.
//
// -----------------------------------------------------------------------------
// The honest limits of the instrument
// -----------------------------------------------------------------------------
//
//   * A timestamp read is not free. On this class of hardware it costs on the
//     order of tens of cycles, so a phase consisting of a handful of
//     instructions cannot be resolved -- the probe would cost more than the
//     work. Phases here are therefore placed at coarse boundaries, and the
//     number of probe pairs is reported alongside every phase so the reader can
//     see how much of a phase's total could be instrument.
//
//   * `__rdtsc` is not a serialising instruction, so a read can be reordered by
//     a few instructions relative to the code around it. Over the millions of
//     samples a benchmark produces that is noise; for a single short phase it
//     is not. No serialising fence is inserted, because `lfence` would itself
//     cost more than some of the phases being measured.
//
//   * Instrumentation perturbs the thing it measures. The profiling build is
//     therefore a SEPARATE binary, so the timed benchmark never carries probes,
//     and the profiling run reports its own total against the uninstrumented
//     total so the inflation is visible rather than assumed small.
//
// When LPQ_ENABLE_PHASE_PROFILING is 0 (the default) every macro below expands
// to nothing and the profiler holds no state, which is ESD.md section 4.4's
// zero-cost-disabled principle applied to instrumentation.
// =============================================================================
#ifndef LPQ_METRICS_PHASE_PROFILER_HPP
#define LPQ_METRICS_PHASE_PROFILER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef LPQ_ENABLE_PHASE_PROFILING
#define LPQ_ENABLE_PHASE_PROFILING 0
#endif

#if LPQ_ENABLE_PHASE_PROFILING
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define LPQ_HAS_RDTSC 1
#include <x86intrin.h>
#else
#define LPQ_HAS_RDTSC 0
#include <chrono>
#endif
#endif

namespace lpq {

/// The phases queue time is attributed to.
///
/// The top-level phases (kPush, kPop, kFallbackScan) are disjoint: every unit
/// of queue time belongs to exactly one. The remaining phases are SUB-phases of
/// kPop and are therefore contained within it; the reporter subtracts them to
/// derive the unattributed remainder rather than letting the shares sum past
/// 100% unexplained.
enum class QueuePhase : std::uint8_t {
    /// A push into the substrate, including the bound maintenance it does.
    kPush,

    /// A whole extraction, from entry to returned element. Encloses the
    /// sub-phases below.
    kPop,

    /// The classical bucket scan taken when the window cannot be verified.
    /// Disjoint from kPop's sub-phases because it replaces them.
    kFallbackScan,

    // --- sub-phases of kPop --------------------------------------------------

    /// Acquiring the current bucket and its scored front.
    kBucketTraversal,

    /// Building and maintaining the candidate window: begin_extraction, the
    /// insertion-sorted offers, and any expansion.
    kWindowMaintenance,

    /// The verification engine deciding whether the window's best candidate is
    /// safe to extract, plus the correction loop around it.
    kVerificationScan,

    /// Removing the verified entry from the bucket.
    kRemoveFromBucket,

    kCount,
};

[[nodiscard]] inline constexpr std::string_view phase_name(QueuePhase p) noexcept {
    switch (p) {
        case QueuePhase::kPush:              return "push";
        case QueuePhase::kPop:               return "pop (total)";
        case QueuePhase::kFallbackScan:      return "fallback scan";
        case QueuePhase::kBucketTraversal:   return "bucket traversal";
        case QueuePhase::kWindowMaintenance: return "window maintenance";
        case QueuePhase::kVerificationScan:  return "verification scan";
        case QueuePhase::kRemoveFromBucket:  return "remove from bucket";
        case QueuePhase::kCount:             return "?";
    }
    return "?";
}

/// True for phases that are contained within kPop rather than disjoint from it.
[[nodiscard]] inline constexpr bool is_pop_subphase(QueuePhase p) noexcept {
    return p == QueuePhase::kBucketTraversal || p == QueuePhase::kWindowMaintenance ||
           p == QueuePhase::kVerificationScan || p == QueuePhase::kRemoveFromBucket;
}

struct PhaseRecord {
    /// Accumulated timestamp-counter ticks.
    std::uint64_t ticks = 0;

    /// How many times the phase was entered. Reported because it is what makes
    /// the probe cost visible: a phase entered ten million times carries ten
    /// million probe pairs inside its own total.
    std::uint64_t calls = 0;
};

/// Accumulates per-phase totals for one process.
///
/// A single global instance, not thread-local: the benchmark protocol pins one
/// thread to one core, so there is nothing to contend and a thread-local would
/// only add an access cost to every probe.
class PhaseProfile {
public:
    [[nodiscard]] static PhaseProfile& instance() noexcept {
        static PhaseProfile profile;
        return profile;
    }

    void add(QueuePhase phase, std::uint64_t ticks) noexcept {
        auto& r = records_[static_cast<std::size_t>(phase)];
        r.ticks += ticks;
        ++r.calls;
    }

    [[nodiscard]] const PhaseRecord& record(QueuePhase phase) const noexcept {
        return records_[static_cast<std::size_t>(phase)];
    }

    void reset() noexcept {
        for (PhaseRecord& r : records_) {
            r = PhaseRecord{};
        }
    }

    /// Ticks spent in the two disjoint top-level phases plus the fallback scan.
    [[nodiscard]] std::uint64_t total_ticks() const noexcept {
        return record(QueuePhase::kPush).ticks + record(QueuePhase::kPop).ticks;
    }

private:
    PhaseProfile() = default;
    /// One slot per phase, plus one for `kCount` itself.
    ///
    /// The extra slot is a sink: the probe-cost calibration charges its ticks
    /// to `kCount` so that measuring the instrument cannot land in a phase the
    /// report shows. `build_phase_report` stops before it, so the sink is
    /// accumulated and never read.
    std::array<PhaseRecord, static_cast<std::size_t>(QueuePhase::kCount) + 1U> records_{};
};

namespace detail {

/// Reads the timestamp counter, or a steady clock where there is none.
[[nodiscard]] inline std::uint64_t read_ticks() noexcept {
#if LPQ_ENABLE_PHASE_PROFILING
#if LPQ_HAS_RDTSC
    return static_cast<std::uint64_t>(__rdtsc());
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
#else
    return 0;
#endif
}

}  // namespace detail

// -----------------------------------------------------------------------------
// Probe macros
// -----------------------------------------------------------------------------
//
// Two forms, because the phases inside `pop` are not all natural scopes. A
// scoped form covers a whole function body; an explicit begin/end pair covers a
// region that ends partway through a block, where introducing a scope would
// have meant moving declarations out of it -- a restructuring of the code under
// measurement, which is exactly what must not happen while the algorithm is
// being held fixed as a baseline.
//
// Both expand to nothing when profiling is off. Not to an empty object: to
// nothing at all, so the disabled build has no local to elide and no reliance
// on the optimiser to remove one.

#define LPQ_PP_CAT_(a, b) a##b
#define LPQ_PP_CAT(a, b) LPQ_PP_CAT_(a, b)

#if LPQ_ENABLE_PHASE_PROFILING

/// Accumulates the ticks between construction and destruction into a phase.
class ScopedPhase {
public:
    explicit ScopedPhase(QueuePhase phase) noexcept
        : phase_(phase), start_(detail::read_ticks()) {}

    ~ScopedPhase() noexcept {
        PhaseProfile::instance().add(phase_, detail::read_ticks() - start_);
    }

    ScopedPhase(const ScopedPhase&)            = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
    ScopedPhase(ScopedPhase&&)                 = delete;
    ScopedPhase& operator=(ScopedPhase&&)      = delete;

private:
    QueuePhase    phase_;
    std::uint64_t start_;
};

#define LPQ_PROFILE_SCOPE(phase) \
    ::lpq::ScopedPhase LPQ_PP_CAT(lpq_scoped_phase_, __LINE__) { phase }

#define LPQ_PROFILE_BEGIN(tag) \
    const std::uint64_t LPQ_PP_CAT(lpq_phase_start_, tag) = ::lpq::detail::read_ticks()

#define LPQ_PROFILE_END(tag, phase)         \
    ::lpq::PhaseProfile::instance().add(    \
        (phase), ::lpq::detail::read_ticks() - LPQ_PP_CAT(lpq_phase_start_, tag))

#else

#define LPQ_PROFILE_SCOPE(phase) static_assert(true, "")
#define LPQ_PROFILE_BEGIN(tag) static_assert(true, "")
#define LPQ_PROFILE_END(tag, phase) static_assert(true, "")

#endif

// -----------------------------------------------------------------------------
// Allocation counting
// -----------------------------------------------------------------------------

/// Counts allocations, which unlike timings is an EXACT quantity.
///
/// Kept separate from the phase timers on purpose. An allocation count needs no
/// clock, carries no probe cost worth mentioning, and is reproducible to the
/// last unit, so it is the one part of this profile that can be stated as fact
/// rather than as a measurement with noise. The counting itself is installed by
/// the profiling binary via a global operator new, not by this header, so that
/// linking this header never changes a program's allocator.
class AllocationCounter {
public:
    [[nodiscard]] static AllocationCounter& instance() noexcept {
        static AllocationCounter counter;
        return counter;
    }

    void note_allocation(std::size_t bytes) noexcept {
        if (!enabled_) {
            return;
        }
        ++allocations_;
        bytes_ += bytes;
    }

    void note_deallocation() noexcept {
        if (!enabled_) {
            return;
        }
        ++deallocations_;
    }

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void reset() noexcept {
        allocations_   = 0;
        deallocations_ = 0;
        bytes_         = 0;
    }

    [[nodiscard]] std::uint64_t allocations() const noexcept { return allocations_; }
    [[nodiscard]] std::uint64_t deallocations() const noexcept { return deallocations_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    AllocationCounter() = default;

    bool          enabled_       = false;
    std::uint64_t allocations_   = 0;
    std::uint64_t deallocations_ = 0;
    std::uint64_t bytes_         = 0;
};

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------

struct PhaseShare {
    QueuePhase    phase;
    std::uint64_t ticks;
    std::uint64_t calls;

    /// Share of the disjoint top-level total (push + pop).
    double share_of_total;

    /// For a sub-phase of pop, its share of pop rather than of everything.
    double share_of_pop;
};

struct PhaseReport {
    std::vector<PhaseShare> shares;

    std::uint64_t total_ticks = 0;
    std::uint64_t pop_ticks   = 0;

    /// Pop time not attributed to any sub-phase. Reported explicitly rather
    /// than distributed across the others, because an unexplained remainder is
    /// a finding and hiding it would make the attribution look complete when it
    /// is not.
    std::uint64_t pop_unattributed_ticks = 0;
    double        pop_unattributed_share = 0.0;

    std::uint64_t probe_pairs = 0;

    std::uint64_t allocations   = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t allocated_bytes = 0;
};

[[nodiscard]] inline PhaseReport build_phase_report() {
    const PhaseProfile& p = PhaseProfile::instance();

    PhaseReport report;
    report.total_ticks = p.total_ticks();
    report.pop_ticks   = p.record(QueuePhase::kPop).ticks;

    std::uint64_t subphase_ticks = 0;

    for (std::size_t i = 0; i < static_cast<std::size_t>(QueuePhase::kCount); ++i) {
        const auto        phase = static_cast<QueuePhase>(i);
        const PhaseRecord r     = p.record(phase);
        report.probe_pairs += r.calls;

        if (is_pop_subphase(phase)) {
            subphase_ticks += r.ticks;
        }

        PhaseShare share;
        share.phase          = phase;
        share.ticks          = r.ticks;
        share.calls          = r.calls;
        share.share_of_total =
            report.total_ticks == 0
                ? 0.0
                : static_cast<double>(r.ticks) / static_cast<double>(report.total_ticks);
        share.share_of_pop =
            report.pop_ticks == 0
                ? 0.0
                : static_cast<double>(r.ticks) / static_cast<double>(report.pop_ticks);
        report.shares.push_back(share);
    }

    // The fallback scan sits inside pop as well, since pop calls it. Counted
    // into the remainder rather than double-counted as a sub-phase.
    subphase_ticks += p.record(QueuePhase::kFallbackScan).ticks;

    report.pop_unattributed_ticks =
        report.pop_ticks > subphase_ticks ? report.pop_ticks - subphase_ticks : 0;
    report.pop_unattributed_share =
        report.pop_ticks == 0 ? 0.0
                              : static_cast<double>(report.pop_unattributed_ticks) /
                                    static_cast<double>(report.pop_ticks);

    const AllocationCounter& a = AllocationCounter::instance();
    report.allocations         = a.allocations();
    report.deallocations       = a.deallocations();
    report.allocated_bytes     = a.bytes();

    return report;
}

}  // namespace lpq

#endif  // LPQ_METRICS_PHASE_PROFILER_HPP
