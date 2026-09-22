// =============================================================================
// Timing primitives (ESD.md section 19: "timers must use a monotonic
// high-resolution clock").
//
// std::chrono::steady_clock is the monotonic clock; system_clock is not and
// must never be used for measurement because an NTP step mid-benchmark would
// silently corrupt a result. The whole engine reports durations in nanoseconds
// as uint64 so the result schema (ESD.md section 23) carries no floating-point
// rounding of its own.
// =============================================================================
#ifndef LPQ_COMMON_CLOCK_HPP
#define LPQ_COMMON_CLOCK_HPP

#include <chrono>
#include <cstdint>

namespace lpq {

using SteadyClock = std::chrono::steady_clock;
using TimePoint   = SteadyClock::time_point;

static_assert(SteadyClock::is_steady,
              "steady_clock must be monotonic for benchmark timing to be valid");

[[nodiscard]] inline TimePoint now() noexcept { return SteadyClock::now(); }

/// Nanoseconds elapsed from `start` to `end`.
[[nodiscard]] inline std::uint64_t elapsed_ns(TimePoint start, TimePoint end) noexcept {
    const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return d < 0 ? 0U : static_cast<std::uint64_t>(d);
}

/// Nanoseconds elapsed from `start` until now.
[[nodiscard]] inline std::uint64_t elapsed_ns_since(TimePoint start) noexcept {
    return elapsed_ns(start, now());
}

/// Accumulates elapsed time into a counter for the lifetime of the scope.
///
/// Used for the feature-extraction / inference / verification timers. The
/// accumulator is a plain reference rather than an atomic because the measured
/// loop is single-threaded by design (PRD.md section 18).
class ScopedTimer {
public:
    explicit ScopedTimer(std::uint64_t& accumulator_ns) noexcept
        : accumulator_(accumulator_ns), start_(now()) {}

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&)                 = delete;
    ScopedTimer& operator=(ScopedTimer&&)      = delete;

    ~ScopedTimer() { accumulator_ += elapsed_ns_since(start_); }

private:
    std::uint64_t& accumulator_;
    TimePoint      start_;
};

/// Nominal resolution implied by the clock's representation, in nanoseconds.
///
/// This is what `steady_clock::period` advertises, NOT what the clock actually
/// delivers. On Windows with libstdc++ the period is 1/1e9, implying 1 ns,
/// while the underlying counter in practice advances in 100 ns steps. Use
/// `effective_clock_resolution_ns()` for any decision about whether a
/// measurement is trustworthy; this exists only to record the declared type.
[[nodiscard]] inline double nominal_clock_resolution_ns() noexcept {
    using P = SteadyClock::period;
    return static_cast<double>(P::num) * 1e9 / static_cast<double>(P::den);
}

/// Measured smallest non-zero interval the clock can report, in nanoseconds.
///
/// The nominal period is misleading -- see above -- and relying on it made the
/// benchmark harness's "this timing is near the clock floor" warning inert on
/// Windows, so sub-resolution timings would have been reported as if they were
/// precise. This probes the clock instead: it calls now() repeatedly and takes
/// the smallest non-zero difference observed.
///
/// Computed once on first use and cached, because the probe itself costs a
/// fraction of a millisecond and must never run inside a timed region.
[[nodiscard]] inline double effective_clock_resolution_ns() noexcept {
    static const double kResolution = [] {
        std::int64_t smallest = 0;
        // A few thousand samples is ample: consecutive now() calls differ by
        // either zero or one tick, so the minimum non-zero delta converges
        // almost immediately.
        for (int i = 0; i < 20000; ++i) {
            const auto a = SteadyClock::now();
            const auto b = SteadyClock::now();
            const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
            if (d > 0 && (smallest == 0 || d < smallest)) {
                smallest = d;
            }
        }
        // Fall back to the nominal period if the probe never saw the clock
        // advance, which would itself indicate an unusable clock.
        return smallest > 0 ? static_cast<double>(smallest) : nominal_clock_resolution_ns();
    }();
    return kResolution;
}

/// Backwards-compatible name; returns the *effective* resolution, which is the
/// one every caller actually wants.
[[nodiscard]] inline double clock_resolution_ns() noexcept {
    return effective_clock_resolution_ns();
}

}  // namespace lpq

#endif  // LPQ_COMMON_CLOCK_HPP
