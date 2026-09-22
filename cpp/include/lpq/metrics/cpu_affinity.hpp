// =============================================================================
// CPU affinity pinning for benchmarks (ESD.md section 20).
//
// ESD.md section 20 says "benchmark CPU affinity should be configurable". On
// the development host it is not merely configurable but mandatory, and the
// reason is worth stating precisely because it determines whether the
// measurements mean anything at all.
//
// The host CPU is an Intel Core i5-1235U: a hybrid part with 2 performance
// (P) cores and 8 efficiency (E) cores. P and E cores differ substantially in
// instructions-per-cycle and in clock. The OS scheduler moves threads between
// them freely in response to load, thermals and power policy, and a laptop part
// under a sustained benchmark is exactly the situation that provokes such a
// move. A run migrated from a P core to an E core mid-measurement is several
// times slower for reasons that have nothing to do with the algorithm.
//
// The effect sizes this project is trying to resolve -- whether LPQ's queue
// savings exceed its ML overhead -- are far smaller than a P-to-E migration.
// Without pinning, the noise would swamp the signal, and worse, it would do so
// asymmetrically: whichever algorithm happened to be scheduled during a
// throttling episode would look bad.
//
// Pinning to ONE core, not merely to a core type, additionally keeps the L1 and
// L2 caches warm across repetitions and removes cross-core migration cost.
//
// The chosen core is recorded in every result record, so two results are
// comparable only when they name the same core, and a reader can tell.
// =============================================================================
#ifndef LPQ_METRICS_CPU_AFFINITY_HPP
#define LPQ_METRICS_CPU_AFFINITY_HPP

#include <cstdint>
#include <string>

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__linux__)
#    include <pthread.h>
#    include <sched.h>
#    include <unistd.h>
#endif

namespace lpq {

/// Outcome of an attempt to pin the current thread.
struct AffinityResult {
    bool        pinned = false;
    /// The core the thread was pinned to, or -1.
    int         core = -1;
    /// Why pinning failed, or why it was not attempted. Empty on success.
    std::string detail;

    [[nodiscard]] std::string describe() const {
        if (pinned) {
            return "pinned to core " + std::to_string(core);
        }
        return "NOT PINNED (" + detail + ")";
    }
};

/// Number of logical processors visible to the process.
[[nodiscard]] inline unsigned logical_processor_count() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<unsigned>(si.dwNumberOfProcessors);
#elif defined(__linux__)
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned>(n) : 1U;
#else
    return 1U;
#endif
}

/// Pins the calling thread to a single logical core.
///
/// Returns a result rather than throwing: a benchmark that cannot pin must
/// still run, but must say so loudly in its output so the reader knows the
/// numbers carry migration noise. Silently proceeding unpinned would produce
/// results indistinguishable from pinned ones in the file while being far less
/// trustworthy.
[[nodiscard]] inline AffinityResult pin_current_thread_to_core(int core) {
    AffinityResult r;
    r.core = core;

    const auto n = static_cast<int>(logical_processor_count());
    if (core < 0 || core >= n) {
        r.detail = "core " + std::to_string(core) + " is outside the range [0, " +
                   std::to_string(n) + ")";
        r.core = -1;
        return r;
    }

#if defined(_WIN32)
    // A mask with exactly one bit set confines the thread to that processor.
    // SetThreadAffinityMask is used rather than SetThreadIdealProcessor, which
    // is only a hint the scheduler may ignore -- and a hint is precisely what
    // is not wanted here.
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    const DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (prev == 0) {
        r.detail = "SetThreadAffinityMask failed with error " +
                   std::to_string(static_cast<unsigned long>(GetLastError()));
        return r;
    }
    r.pinned = true;
    return r;

#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(core), &set);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
    if (rc != 0) {
        // Common in containers with a restricted cpuset, which is a real
        // possibility for this project's Docker-based Linux runs.
        r.detail = "pthread_setaffinity_np failed with errno " + std::to_string(rc) +
                   " (a container cpuset may exclude this core)";
        return r;
    }
    r.pinned = true;
    return r;

#else
    r.detail = "CPU pinning is not implemented for this platform";
    return r;
#endif
}

/// Raises the current thread's scheduling priority for the duration of a
/// benchmark.
///
/// Reduces preemption by unrelated work. Deliberately does NOT request
/// real-time priority: on a desktop system that risks starving the scheduler
/// and freezing the machine, and the gain over "above normal" is small compared
/// with the gain from pinning.
///
/// Returns false when the privilege is unavailable, which is not an error.
inline bool raise_benchmark_thread_priority() noexcept {
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
#else
    // Lowering `nice` needs CAP_SYS_NICE, which an ordinary container or user
    // session lacks. Pinning is the load-bearing control; this is a bonus.
    return false;
#endif
}

/// Default core for benchmarks.
///
/// Core 0 is avoided: on both Windows and Linux it attracts a
/// disproportionate share of interrupt and kernel housekeeping work. Core 1 is
/// used instead, and on the hybrid host it is the second logical processor of
/// the first P-core, which is the intended target.
[[nodiscard]] inline int default_benchmark_core() noexcept {
    return logical_processor_count() > 1U ? 1 : 0;
}

}  // namespace lpq

#endif  // LPQ_METRICS_CPU_AFFINITY_HPP
