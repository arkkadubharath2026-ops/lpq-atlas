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

/// Resolution of the steady clock in nanoseconds.
///
/// Recorded in result metadata so that a reader can tell whether a reported
/// per-operation time is above or below the platform's measurement floor --
/// on Windows this is typically 100 ns, on Linux typically 1 ns.
[[nodiscard]] inline double clock_resolution_ns() noexcept {
    using P = SteadyClock::period;
    return static_cast<double>(P::num) * 1e9 / static_cast<double>(P::den);
}

}  // namespace lpq

#endif  // LPQ_COMMON_CLOCK_HPP
