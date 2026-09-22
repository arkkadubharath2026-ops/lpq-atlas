// =============================================================================
// Cache-friendly static graph (ESD.md section 5).
//
// LAYOUT DECISION -- deviation from the ESD's illustrative layout, recorded
// here and in docs/architecture/graph_representation.md.
//
// ESD.md section 5 sketches the representation as three parallel arrays:
//
//     offsets[]  edges[]  weights[]
//
// and calls it a "preferred default ... equivalent to a CSR-like
// representation". This implementation keeps offsets[] but *interleaves* the
// target and the weight into a single array of Arc{target, weight} records:
//
//     offsets[]  arcs[]           where arcs[e] = {target, weight}
//
// It is still CSR -- same indexing, same asymptotics, same iteration order --
// but the relaxation loop reads both fields of every arc it visits, and
// interleaving means one sequential stream and one cache line per 8 arcs
// (uint32 target + uint32 weight) instead of two streams and two partially
// used cache lines. On road networks, where the mean out-degree is about 2-3,
// a node's entire adjacency usually fits in a single cache line this way,
// whereas the parallel-array form touches two lines to read three arcs. The
// total bytes are identical; what changes is how many lines and how many
// streams the hot loop touches.
//
// The graph is immutable once built. Mutation would invalidate the metadata,
// the degree statistics, and any model trained against them, so modified
// graphs (PRD.md section 15, road closures) are produced by building a new
// graph from an edge list rather than by editing one in place.
// =============================================================================
#ifndef LPQ_GRAPH_CSR_GRAPH_HPP
#define LPQ_GRAPH_CSR_GRAPH_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"

namespace lpq {

/// One outgoing arc: destination plus traversal cost.
template <WeightType W>
struct Arc {
    NodeId target;
    W      weight;

    friend constexpr bool operator==(const Arc&, const Arc&) = default;
};

/// Geographic or Euclidean coordinates, present only for spatial graphs.
///
/// Stored separately from the arcs because the shortest-path inner loop never
/// reads them; only the A* heuristic does, and only for the node it is
/// currently keying. Interleaving them into the arc array would evict useful
/// adjacency data from cache for no benefit.
struct Coordinate {
    double x = 0.0;  ///< Longitude for geographic graphs, x for Euclidean ones.
    double y = 0.0;  ///< Latitude for geographic graphs, y for Euclidean ones.

    friend constexpr bool operator==(const Coordinate&, const Coordinate&) = default;
};

/// How to interpret Coordinate values.
enum class CoordinateSystem : std::uint8_t {
    kNone,        ///< No coordinates; only the zero heuristic is admissible.
    kEuclidean,   ///< Plane coordinates; Euclidean and Manhattan heuristics apply.
    kGeographic,  ///< Degrees of longitude/latitude; the Haversine heuristic applies.
};

[[nodiscard]] constexpr const char* to_string(CoordinateSystem c) noexcept {
    switch (c) {
        case CoordinateSystem::kNone:       return "none";
        case CoordinateSystem::kEuclidean:  return "euclidean";
        case CoordinateSystem::kGeographic: return "geographic";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
// CsrGraph
// -----------------------------------------------------------------------------

/// Immutable compressed-sparse-row graph with non-negative arc weights.
///
/// Build instances with GraphBuilder (graph_builder.hpp), which performs the
/// validation this class's invariants depend on. The constructor here is the
/// low-level entry point and revalidates, because a corrupt graph produces
/// wrong shortest paths rather than a crash.
template <WeightType W>
class CsrGraph {
public:
    using WeightT   = W;
    using DistanceT = DistanceType<W>;
    using ArcT      = Arc<W>;

    CsrGraph() = default;

    /// Constructs from prebuilt CSR arrays.
    ///
    /// offsets must have node_count + 1 entries, be non-decreasing, start at 0
    /// and end at arcs.size(). Every arc target must be a valid node id, and
    /// every weight must be non-negative and finite. Violations throw
    /// InvalidGraphError; this is the boundary at which untrusted dataset input
    /// becomes a trusted internal structure.
    CsrGraph(std::vector<EdgeId> offsets, std::vector<ArcT> arcs, Directedness directedness,
             std::vector<Coordinate> coordinates  = {},
             CoordinateSystem        coord_system = CoordinateSystem::kNone,
             std::string             dataset_id   = {})
        : offsets_(std::move(offsets)),
          arcs_(std::move(arcs)),
          coordinates_(std::move(coordinates)),
          directedness_(directedness),
          coord_system_(coord_system),
          dataset_id_(std::move(dataset_id)) {
        validate();
    }

    // --- Size -----------------------------------------------------------------

    [[nodiscard]] std::size_t num_nodes() const noexcept {
        return offsets_.empty() ? 0U : offsets_.size() - 1U;
    }

    [[nodiscard]] std::size_t num_arcs() const noexcept { return arcs_.size(); }

    /// Number of edges in the semantic sense: for an undirected graph this is
    /// half the stored arc count, since each edge is stored as two arcs.
    [[nodiscard]] std::size_t num_edges() const noexcept {
        return directedness_ == Directedness::kUndirected ? arcs_.size() / 2U : arcs_.size();
    }

    [[nodiscard]] bool empty() const noexcept { return num_nodes() == 0U; }

    // --- Adjacency ------------------------------------------------------------

    /// Half-open arc-index range [first, last) of node u's outgoing arcs.
    [[nodiscard]] EdgeId arc_begin(NodeId u) const noexcept { return offsets_[u]; }
    [[nodiscard]] EdgeId arc_end(NodeId u) const noexcept { return offsets_[u + 1U]; }

    [[nodiscard]] std::size_t out_degree(NodeId u) const noexcept {
        return static_cast<std::size_t>(offsets_[u + 1U] - offsets_[u]);
    }

    /// Contiguous view of node u's outgoing arcs.
    ///
    /// This is the hot-loop accessor. It is a view into the single arc array, so
    /// iterating it is a sequential scan with no indirection.
    [[nodiscard]] std::span<const ArcT> out_arcs(NodeId u) const noexcept {
        const EdgeId b = offsets_[u];
        const EdgeId e = offsets_[u + 1U];
        return std::span<const ArcT>(arcs_.data() + b, static_cast<std::size_t>(e - b));
    }

    [[nodiscard]] const ArcT& arc(EdgeId e) const noexcept { return arcs_[e]; }

    // --- Raw arrays (for serialization and bulk analysis) ---------------------

    [[nodiscard]] std::span<const EdgeId> offsets() const noexcept { return offsets_; }
    [[nodiscard]] std::span<const ArcT>   arcs() const noexcept { return arcs_; }

    // --- Spatial --------------------------------------------------------------

    [[nodiscard]] bool has_coordinates() const noexcept {
        return coord_system_ != CoordinateSystem::kNone && !coordinates_.empty();
    }

    [[nodiscard]] CoordinateSystem coordinate_system() const noexcept { return coord_system_; }

    [[nodiscard]] const Coordinate& coordinate(NodeId u) const noexcept { return coordinates_[u]; }

    [[nodiscard]] std::span<const Coordinate> coordinates() const noexcept { return coordinates_; }

    // --- Identity -------------------------------------------------------------

    [[nodiscard]] Directedness       directedness() const noexcept { return directedness_; }
    [[nodiscard]] const std::string& dataset_id() const noexcept { return dataset_id_; }

    void set_dataset_id(std::string id) { dataset_id_ = std::move(id); }

    [[nodiscard]] static constexpr WeightKind weight_kind() noexcept {
        return weight_kind_of<W>();
    }

    /// Bytes of heap storage held by the CSR arrays.
    ///
    /// Reported alongside benchmark results so that a runtime difference can be
    /// checked against a memory-footprint difference.
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return offsets_.capacity() * sizeof(EdgeId) + arcs_.capacity() * sizeof(ArcT) +
               coordinates_.capacity() * sizeof(Coordinate);
    }

private:
    void validate() const {
        if (offsets_.empty()) {
            if (!arcs_.empty()) {
                throw InvalidGraphError("arcs present but offsets array is empty");
            }
            return;  // A zero-node graph is legal and useful in edge-case tests.
        }
        if (offsets_.front() != 0U) {
            throw InvalidGraphError("offsets[0] must be 0");
        }
        if (offsets_.back() != static_cast<EdgeId>(arcs_.size())) {
            throw InvalidGraphError("offsets.back() must equal the arc count");
        }
        const std::size_t n = num_nodes();
        if (n > kMaxNodeCount) {
            throw InvalidGraphError("node count exceeds the NodeId range");
        }
        for (std::size_t i = 1; i < offsets_.size(); ++i) {
            if (offsets_[i] < offsets_[i - 1U]) {
                throw InvalidGraphError("offsets array is not non-decreasing");
            }
        }
        for (const ArcT& a : arcs_) {
            if (static_cast<std::size_t>(a.target) >= n) {
                throw InvalidGraphError("arc target " + std::to_string(a.target) +
                                        " is out of range for " + std::to_string(n) + " nodes");
            }
            if constexpr (std::same_as<W, double>) {
                // Dijkstra's correctness argument requires non-negative weights.
                // NaN is rejected too: it makes every comparison false and would
                // silently break heap ordering rather than fail loudly.
                if (!(a.weight >= 0.0)) {
                    throw InvalidGraphError("arc weight is negative or NaN");
                }
                if (a.weight == std::numeric_limits<double>::infinity()) {
                    throw InvalidGraphError("arc weight is infinite");
                }
            }
        }
        if (!coordinates_.empty() && coordinates_.size() != n) {
            throw InvalidGraphError("coordinate array size does not match the node count");
        }
        if (coord_system_ != CoordinateSystem::kNone && coordinates_.empty() && n > 0U) {
            throw InvalidGraphError("coordinate system declared but no coordinates supplied");
        }
    }

    std::vector<EdgeId>     offsets_;      ///< size == num_nodes() + 1
    std::vector<ArcT>       arcs_;         ///< size == num_arcs()
    std::vector<Coordinate> coordinates_;  ///< empty, or size == num_nodes()

    Directedness     directedness_ = Directedness::kDirected;
    CoordinateSystem coord_system_ = CoordinateSystem::kNone;
    std::string      dataset_id_;
};

// Concrete instantiations used throughout the project.
using GraphU32 = CsrGraph<std::uint32_t>;
using GraphU64 = CsrGraph<std::uint64_t>;
using GraphF64 = CsrGraph<double>;

}  // namespace lpq

#endif  // LPQ_GRAPH_CSR_GRAPH_HPP
