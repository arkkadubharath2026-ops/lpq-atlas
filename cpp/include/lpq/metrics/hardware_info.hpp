// =============================================================================
// Build and hardware provenance (ESD.md sections 20, 23, 40).
//
// ESD.md section 40: "No benchmark result is generated without hardware and git
// metadata." This header is what makes that enforceable -- every result record
// embeds the environment that produced it, so a number can never be compared
// against another number from a different machine, compiler or build
// configuration without that difference being visible.
//
// Compiler identity, optimization level and instrumentation state are captured
// from preprocessor macros at compile time, so they describe the binary that is
// actually running rather than whatever the build scripts last intended.
//
// HARDWARE COUNTERS ARE NOT COLLECTED HERE. The development host runs under
// WSL2, which exposes no CPU PMU, so cycles, instructions and cache misses are
// unavailable; see docs/experiments/profiling.md. The result schema encodes
// them as JSON null rather than zero, because a reader must be able to tell
// "not measured" from "measured as zero". No value is ever estimated.
// =============================================================================
#ifndef LPQ_METRICS_HARDWARE_INFO_HPP
#define LPQ_METRICS_HARDWARE_INFO_HPP

#include <cstdint>
#include <string>

#include "lpq/common/clock.hpp"
#include "lpq/metrics/cpu_affinity.hpp"

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__linux__)
#    include <fstream>
#    include <sys/utsname.h>
#    include <unistd.h>
#endif

namespace lpq {

/// Environment in which a benchmark ran.
struct HardwareInfo {
    std::string cpu_model    = "unknown";
    unsigned    logical_cores = 0;
    std::uint64_t total_ram_bytes = 0;
    std::string os            = "unknown";

    std::string compiler         = "unknown";
    std::string compiler_version = "unknown";
    std::string build_type       = "unknown";
    std::string optimization_flags = "unknown";

    bool   instrumentation_enabled = false;
    bool   native_arch             = false;
    double clock_resolution_ns     = 0.0;

    /// Git commit of the working tree, injected by the build system. "unknown"
    /// when the build had no git information, which the result validator
    /// treats as a hard error for publication-tier runs.
    std::string git_commit = "unknown";
    /// True when the working tree had uncommitted changes at build time. A
    /// dirty tree means the commit hash does not fully identify the code, so
    /// results from it are not reproducible and must be flagged.
    bool git_dirty = false;

    /// Whether PMU hardware counters are obtainable on this host. Always false
    /// under WSL2. Recorded so the absence of counters in a result is
    /// explained rather than merely observed.
    bool hardware_counters_available = false;
};

namespace detail {

[[nodiscard]] inline std::string compiler_name() {
#if defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#elif defined(_MSC_VER)
    return "msvc";
#else
    return "unknown";
#endif
}

[[nodiscard]] inline std::string compiler_version_string() {
#if defined(__clang__)
    return std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
           std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

/// Derived from NDEBUG rather than from a build-system string, so it reflects
/// the binary rather than the intent.
[[nodiscard]] inline std::string build_type_string() {
#if defined(NDEBUG)
    return "release";
#else
    return "debug";
#endif
}

#if defined(_WIN32)
[[nodiscard]] inline std::string windows_cpu_model() {
    HKEY  key{};
    char  buffer[256] = {};
    DWORD size        = sizeof(buffer);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        const LSTATUS rc = RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                                            reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS) {
            std::string s(buffer);
            // The registry value is space-padded.
            while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
                s.pop_back();
            }
            return s;
        }
    }
    return "unknown";
}
#endif

#if defined(__linux__)
[[nodiscard]] inline std::string linux_cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string   line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto s = line.substr(colon + 1);
                while (!s.empty() && s.front() == ' ') {
                    s.erase(s.begin());
                }
                return s;
            }
        }
    }
    return "unknown";
}

/// True when a `cpu` PMU is exposed. Under WSL2 it is not, so this returns
/// false and the schema records counters as null.
[[nodiscard]] inline bool linux_pmu_available() {
    // The hybrid-core names are checked too, since on an Alder Lake host a
    // native Linux kernel presents cpu_core and cpu_atom rather than cpu.
    for (const char* name : {"/sys/bus/event_source/devices/cpu",
                             "/sys/bus/event_source/devices/cpu_core",
                             "/sys/bus/event_source/devices/cpu_atom"}) {
        if (access(name, F_OK) == 0) {
            return true;
        }
    }
    return false;
}
#endif

}  // namespace detail

/// Collects the environment. Called once per benchmark run, never in a timed
/// region.
[[nodiscard]] inline HardwareInfo collect_hardware_info() {
    HardwareInfo h;

    h.logical_cores       = logical_processor_count();
    h.compiler            = detail::compiler_name();
    h.compiler_version    = detail::compiler_version_string();
    h.build_type          = detail::build_type_string();
    h.clock_resolution_ns = clock_resolution_ns();

#if LPQ_ENABLE_INSTRUMENTATION
    h.instrumentation_enabled = true;
#else
    h.instrumentation_enabled = false;
#endif

    // Injected by CMake; see cmake/LpqBuildInfo.cmake.
#ifdef LPQ_GIT_COMMIT
    h.git_commit = LPQ_GIT_COMMIT;
#endif
#ifdef LPQ_GIT_DIRTY
    h.git_dirty = (LPQ_GIT_DIRTY != 0);
#endif
#ifdef LPQ_OPTIMIZATION_FLAGS
    h.optimization_flags = LPQ_OPTIMIZATION_FLAGS;
#endif
#ifdef LPQ_NATIVE_ARCH_ENABLED
    h.native_arch = (LPQ_NATIVE_ARCH_ENABLED != 0);
#endif

#if defined(_WIN32)
    h.cpu_model = detail::windows_cpu_model();
    h.os        = "windows";
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        h.total_ram_bytes = static_cast<std::uint64_t>(ms.ullTotalPhys);
    }
    // Windows exposes PMU counters only through ETW or vendor tools, not
    // through anything this process can read directly, so counters are
    // reported unavailable rather than guessed at.
    h.hardware_counters_available = false;

#elif defined(__linux__)
    h.cpu_model = detail::linux_cpu_model();
    utsname u{};
    if (uname(&u) == 0) {
        h.os = std::string(u.sysname) + " " + u.release;
    } else {
        h.os = "linux";
    }
    const long pages     = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        h.total_ram_bytes =
            static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
    }
    h.hardware_counters_available = detail::linux_pmu_available();
#endif

    return h;
}

}  // namespace lpq

#endif  // LPQ_METRICS_HARDWARE_INFO_HPP
