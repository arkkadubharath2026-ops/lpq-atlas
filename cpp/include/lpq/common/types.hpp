// =============================================================================
// Core scalar types shared by the whole engine.
//
// Width choices are deliberate and affect both correctness and cache behaviour:
//
//   NodeId  = uint32_t  Addresses up to ~4.29e9 nodes. Every real dataset in
//                       PRD.md section 17 (SNAP, OSM, synthetic) fits, and
//                       halving the index width against uint64_t halves the
//                       footprint of the CSR edge array, the distance array and
//                       the heap position map -- the three structures that
//                       dominate cache traffic in Dijkstra.
//   EdgeId  = uint64_t  Edge counts can exceed 2^32 on dense synthetic graphs,
//                       and the offsets array is one entry per node, so the
//                       wider type costs far less than the node arrays.
//
// Sentinels are the maximum value of their type rather than -1 so that the
// types can stay unsigned; unsigned indices remove a whole class of negative
// -index undefined behaviour from the hot loops.
// =============================================================================
#ifndef LPQ_COMMON_TYPES_HPP
#define LPQ_COMMON_TYPES_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lpq {

using NodeId = std::uint32_t;
using EdgeId = std::uint64_t;

/// Sentinel node: "no such node". Used for absent predecessors and unset targets.
inline constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();

/// Sentinel edge: "no such edge".
inline constexpr EdgeId kInvalidEdge = std::numeric_limits<EdgeId>::max();

/// Maximum addressable node count (one value is reserved for the sentinel).
inline constexpr std::size_t kMaxNodeCount = static_cast<std::size_t>(kInvalidNode);

// -----------------------------------------------------------------------------
// Weight concept
//
// The engine is generic over the weight type so that integer-weight benchmarks
// can use exact integer arithmetic (ESD.md section 25) while geographic and
// synthetic graphs can use double. Only these two families are permitted:
// signed integers are excluded because Dijkstra requires non-negative weights
// and an unsigned type makes that a type-level guarantee rather than a runtime
// check, and floats narrower than double are excluded because accumulated path
// costs over long road-network paths lose too much precision in 32 bits.
// -----------------------------------------------------------------------------
template <typename T>
concept WeightType =
    (std::unsigned_integral<T> && !std::same_as<T, bool>) || std::same_as<T, double>;

/// Integer weights: distances accumulate in a wider type to avoid overflow.
template <typename W>
struct DistanceTraitsImpl;

template <std::unsigned_integral W>
struct DistanceTraitsImpl<W> {
    /// Path costs are summed in 64 bits regardless of the edge-weight width, so
    /// a long path over uint32 weights cannot silently wrap.
    using type = std::uint64_t;
};

template <>
struct DistanceTraitsImpl<double> {
    using type = double;
};

/// Accumulator type for path costs built from weights of type `W`.
template <WeightType W>
using DistanceType = typename DistanceTraitsImpl<W>::type;

/// "Unreachable" distance for weight type `W`.
///
/// For integers this is the maximum representable value. For doubles it is
/// +infinity, which compares correctly against every finite cost and propagates
/// through arithmetic instead of wrapping.
template <WeightType W>
inline constexpr DistanceType<W> kUnreachable = [] {
    if constexpr (std::same_as<W, double>) {
        return std::numeric_limits<double>::infinity();
    } else {
        return std::numeric_limits<DistanceType<W>>::max();
    }
}();

/// True when `d` denotes an unreached node.
template <WeightType W>
[[nodiscard]] constexpr bool is_unreachable(DistanceType<W> d) noexcept {
    return d >= kUnreachable<W>;
}

// -----------------------------------------------------------------------------
// Directedness
// -----------------------------------------------------------------------------
enum class Directedness : std::uint8_t {
    kDirected,
    /// Stored as paired directed edges (ESD.md section 5); the flag records the
    /// semantic origin so metadata and query generation can reason about it.
    kUndirected,
};

/// Stable string form used in metadata files and result records.
[[nodiscard]] constexpr const char* to_string(Directedness d) noexcept {
    return d == Directedness::kDirected ? "directed" : "undirected";
}

// -----------------------------------------------------------------------------
// Weight kind: the runtime tag written into dataset metadata, mirroring the
// compile-time `WeightType` used by the templates.
// -----------------------------------------------------------------------------
enum class WeightKind : std::uint8_t { kUInt32, kUInt64, kDouble };

[[nodiscard]] constexpr const char* to_string(WeightKind k) noexcept {
    switch (k) {
        case WeightKind::kUInt32: return "uint32";
        case WeightKind::kUInt64: return "uint64";
        case WeightKind::kDouble: return "double";
    }
    return "unknown";
}

/// Maps a compile-time weight type to its runtime tag.
template <WeightType W>
[[nodiscard]] constexpr WeightKind weight_kind_of() noexcept {
    if constexpr (std::same_as<W, double>) {
        return WeightKind::kDouble;
    } else if constexpr (sizeof(W) <= 4) {
        return WeightKind::kUInt32;
    } else {
        return WeightKind::kUInt64;
    }
}

}  // namespace lpq

#endif  // LPQ_COMMON_TYPES_HPP
