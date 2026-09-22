// =============================================================================
// Error types.
//
// ESD.md section 37 requires that malformed graph files be rejected and that
// configuration values be bounded. Input validation therefore throws a typed
// exception rather than aborting or, worse, proceeding with a corrupt graph:
// a silently truncated edge list would produce shortest paths that are wrong
// but plausible, which is the single most dangerous failure mode for a
// measurement system.
//
// Exceptions are confined to setup and teardown -- loading, validation,
// configuration. They never appear in the timed shortest-path loop.
// =============================================================================
#ifndef LPQ_COMMON_ERROR_HPP
#define LPQ_COMMON_ERROR_HPP

#include <stdexcept>
#include <string>
#include <utility>

namespace lpq {

/// Base class for every error this library raises.
class LpqError : public std::runtime_error {
public:
    explicit LpqError(const std::string& what) : std::runtime_error(what) {}
};

/// A dataset file is missing, unreadable, truncated, or violates the format.
class InvalidGraphError : public LpqError {
public:
    explicit InvalidGraphError(const std::string& what)
        : LpqError("invalid graph: " + what) {}
};

/// A configuration value is outside its permitted range.
class InvalidConfigError : public LpqError {
public:
    explicit InvalidConfigError(const std::string& what)
        : LpqError("invalid configuration: " + what) {}
};

/// A requested algorithm/queue/mode combination is not supported.
///
/// Raised, for example, when a radix heap is requested for a floating-point or
/// non-monotonic workload. ESD.md section 7 requires such combinations to be
/// reported as unsupported rather than silently producing invalid results.
class UnsupportedConfigurationError : public LpqError {
public:
    explicit UnsupportedConfigurationError(const std::string& what)
        : LpqError("unsupported configuration: " + what) {}
};

/// An internal invariant was violated. Indicates a bug in this library.
class InternalError : public LpqError {
public:
    explicit InternalError(const std::string& what)
        : LpqError("internal error: " + what) {}
};

namespace detail {
/// Throws `InvalidConfigError` when `value` is outside `[lo, hi]`.
template <typename T>
T check_range(T value, T lo, T hi, const char* name) {
    if (value < lo || value > hi) {
        throw InvalidConfigError(std::string(name) + " = " + std::to_string(value) +
                                 " is outside [" + std::to_string(lo) + ", " +
                                 std::to_string(hi) + "]");
    }
    return value;
}
}  // namespace detail

}  // namespace lpq

#endif  // LPQ_COMMON_ERROR_HPP
