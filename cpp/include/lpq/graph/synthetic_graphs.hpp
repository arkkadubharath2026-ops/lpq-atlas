// =============================================================================
// Synthetic graph generation (PRD.md section 17, ESD.md section 28).
//
// ESD.md section 28 asks for one generator covering six families -- grid,
// Erdos-Renyi, Barabasi-Albert, random geometric, Poisson Voronoi and
// road-like synthetic. They exist because the priority queues under test are
// not equally sensitive to graph shape, in the same way that
// weight_distributions.hpp exists because they are not equally sensitive to
// weight shape. A binary heap and a bucket queue separate very differently on a
// lattice with bounded degree and on a power-law graph where a handful of hubs
// dominate the decrease-key traffic, and a single family would silently pick a
// winner.
//
// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------
//
// Every generator draws from one explicitly seeded Rng, and weights come from
// weight_distributions.hpp, which derives each weight from a hash of the
// unordered endpoint pair rather than from a stream. Consequently the same
// SyntheticGraphConfig always yields a byte-identical CsrGraph, on any platform
// with IEEE-754 doubles, and the edge order produced by a generator cannot
// perturb the weights. A generated graph is therefore fully described by its
// config, which is what makes an experiment manifest reproducible
// (ESD.md section 4.5).
//
// -----------------------------------------------------------------------------
// Directedness
// -----------------------------------------------------------------------------
//
// All six families are undirected as defined in the literature. When
// Directedness::kDirected is requested the generator emits BOTH arcs of every
// generated edge and labels the graph directed. That is a bidirected graph, not
// an arbitrary digraph: this header does not claim to generate asymmetric
// digraphs, because an asymmetric random digraph is a different experiment with
// different connectivity behaviour and would need its own family and its own
// validation. The directed label is provided so the same structure can be run
// through the directed code paths.
//
// -----------------------------------------------------------------------------
// Guarding against accidental O(n^2) generation
// -----------------------------------------------------------------------------
//
// Erdos-Renyi with a high p and a large n has a quadratic edge count, and the
// naive implementation also has quadratic RUNTIME even when p is tiny, because
// it flips a coin for every one of the n(n-1)/2 pairs. Both are handled:
//
//   * runtime -- the generator uses the Batagelj-Brandes geometric-skip method,
//     whose cost is proportional to the number of edges actually emitted rather
//     than to the number of candidate pairs, so a sparse graph on a large n is
//     cheap;
//   * memory -- the expected edge count is computed from n and p in validate()
//     and rejected when it exceeds SyntheticGraphConfig::max_edges, so a
//     mistyped p fails at configuration time with a clear message instead of
//     exhausting memory an hour into a run. Generation additionally re-checks
//     the realised count, because the expectation is not a bound.
//
// The same max_edges ceiling is applied to every family, since a random
// geometric graph with too large a radius is the same mistake wearing a
// different name.
//
// -----------------------------------------------------------------------------
// What "Poisson Voronoi" here actually is -- read this before citing it
// -----------------------------------------------------------------------------
//
// The road network associated with a Poisson point process is the adjacency
// graph of the Voronoi cells, which is exactly the Delaunay graph of the
// points. An exact Delaunay triangulation is a substantial piece of
// computational geometry with its own degeneracy and robust-predicate problems,
// and it is not what this file implements.
//
// What is implemented is the GABRIEL GRAPH of a uniform (binomial) point
// sample, computed with a uniform grid index. The Gabriel graph contains an
// edge {p, q} exactly when the closed disk having pq as its diameter holds no
// other point. It is a well-defined graph in its own right and it is a
// SUBGRAPH of the Delaunay triangulation, so every edge it reports is a genuine
// Voronoi-cell adjacency; it never invents one. Two honest limitations follow:
//
//   1. It is a strict subgraph. Delaunay edges whose diametral disk contains
//      another point are absent, so this graph is sparser than a true Voronoi
//      adjacency graph -- typically around two thirds of the Delaunay edges for
//      a uniform sample. It is not a triangulation and must not be described as
//      one.
//   2. Candidate pairs are drawn from a bounded neighbourhood of grid cells
//      (voronoi_cell_neighbourhood). The Gabriel PREDICATE is evaluated exactly,
//      against every point in every cell the diametral disk touches, so no
//      false edge can be produced; but a genuine Gabriel edge spanning a sparse
//      region further than the candidate neighbourhood will be missed. The
//      Gabriel graph is connected in theory (it contains the Euclidean minimum
//      spanning tree); under this candidate restriction connectivity is NOT
//      guaranteed, which is why `ensure_connected` exists.
//
// The family is therefore named kPoissonVoronoiApprox and its canonical string
// is "poisson_voronoi_approx". The ESD.md spelling "poisson_voronoi" is
// accepted as an explicit alias so the CLI of section 28 works as written, but
// the name that gets recorded in a manifest is the accurate one.
//
// The point sample is a fixed-count (binomial) sample in the unit square rather
// than a true Poisson process with a random count, because an experiment
// manifest specifies the node count. For a fixed window the two differ only by
// the conditioning on n.
//
// -----------------------------------------------------------------------------
// What "road-like" here actually is
// -----------------------------------------------------------------------------
//
// Real road networks are near-planar, have low and tightly bounded degree, have
// geometric coordinates that make A* effective, and are hierarchical: a small
// number of arterials carry traffic between dense neighbourhoods of slow local
// streets. kRoadLike reproduces those four properties by construction and
// nothing more -- it is a caricature, not a model fitted to OSM data, and
// results on it are not a substitute for the real OSM datasets PRD.md
// section 17 requires:
//
//   * a jittered lattice supplies planarity and coordinates,
//   * a uniformly random spanning tree of that lattice (randomised DFS) supplies
//     connectivity BY CONSTRUCTION and the irregular, dead-ended look of local
//     streets,
//   * a fraction of the remaining lattice edges is added back to create cycles,
//     because a tree has exactly one path between any pair and would make every
//     shortest-path query trivial,
//   * every road_arterial_period-th row and column is completed and its weights
//     divided by road_arterial_speedup, giving the fast long-range structure
//     that makes road networks respond to A* and to bidirectional search.
//
// -----------------------------------------------------------------------------
// Weights and A* admissibility
// -----------------------------------------------------------------------------
//
// By default weights come from the configured WeightDistributionConfig, which
// is independent of geometry. That is right for measuring queue behaviour but
// it makes the Euclidean A* heuristic inadmissible, since a randomly weighted
// short edge can cost less than its length. Setting `euclidean_weights` instead
// derives each weight from the Euclidean distance between the endpoints times
// `euclidean_weight_scale`; it is permitted only for families that carry
// coordinates, and it is still symmetric and order-independent.
//
// Even then, admissibility is only exact for W = double. For an integer weight
// type quantize_weight() rounds to nearest, which can round an edge weight
// DOWN, and a heuristic scaled identically would then overestimate. Use double
// weights when the experiment depends on A* returning exact shortest paths.
// This is stated rather than papered over; nothing here measures or asserts
// admissibility.
// =============================================================================
#ifndef LPQ_GRAPH_SYNTHETIC_GRAPHS_HPP
#define LPQ_GRAPH_SYNTHETIC_GRAPHS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/weight_distributions.hpp"

namespace lpq {

/// The graph families required by PRD.md section 17 and ESD.md section 28.
enum class GraphFamily : std::uint8_t {
    /// 2D lattice with 4-neighbour connectivity. Bounded degree, large diameter,
    /// integer coordinates.
    kGrid,
    /// G(n, p). Small diameter, Poisson degree distribution, no geometry.
    kErdosRenyi,
    /// Preferential attachment. Power-law degrees, so a few hub nodes dominate
    /// the decrease-key traffic; connected by construction.
    kBarabasiAlbert,
    /// Uniform points in the unit square joined when within a radius. Geometric
    /// like a lattice but with irregular degrees; not necessarily connected.
    kRandomGeometric,
    /// The Gabriel graph of a uniform point sample: a subgraph of the Delaunay
    /// graph of those points, not an exact Voronoi adjacency graph. See the
    /// header comment for exactly what this does and does not contain.
    kPoissonVoronoiApprox,
    /// Jittered lattice, random spanning tree, extra cycles and fast arterials.
    /// A caricature of a road network, not a fitted model.
    kRoadLike,
};

/// Canonical name, as recorded in a manifest.
[[nodiscard]] inline std::string_view graph_family_name(GraphFamily f) noexcept {
    switch (f) {
        case GraphFamily::kGrid:                 return "grid";
        case GraphFamily::kErdosRenyi:           return "erdos_renyi";
        case GraphFamily::kBarabasiAlbert:       return "barabasi_albert";
        case GraphFamily::kRandomGeometric:      return "random_geometric";
        case GraphFamily::kPoissonVoronoiApprox: return "poisson_voronoi_approx";
        case GraphFamily::kRoadLike:             return "road_like";
    }
    return "unknown";
}

/// Parses a family name. Returns false for an unknown name rather than falling
/// back to a default, for the same reason parse_weight_distribution() does: a
/// typo in a manifest must fail loudly instead of quietly changing which
/// experiment was run.
///
/// "poisson_voronoi" is accepted as a deliberate alias of the approximate
/// family so the ESD.md section 28 command line works verbatim; it maps to
/// kPoissonVoronoiApprox, whose canonical name is what is written back out.
[[nodiscard]] inline bool parse_graph_family(std::string_view name, GraphFamily& out) noexcept {
    if (name == "grid")              { out = GraphFamily::kGrid;            return true; }
    if (name == "erdos_renyi")       { out = GraphFamily::kErdosRenyi;      return true; }
    if (name == "barabasi_albert")   { out = GraphFamily::kBarabasiAlbert;  return true; }
    if (name == "random_geometric")  { out = GraphFamily::kRandomGeometric; return true; }
    if (name == "road_like")         { out = GraphFamily::kRoadLike;        return true; }
    if (name == "poisson_voronoi_approx" || name == "poisson_voronoi") {
        out = GraphFamily::kPoissonVoronoiApprox;
        return true;
    }
    return false;
}

/// True for the families that place their nodes in the plane and therefore
/// attach Coordinates, which is what makes A* usable on them.
[[nodiscard]] inline constexpr bool family_has_coordinates(GraphFamily f) noexcept {
    return f == GraphFamily::kGrid || f == GraphFamily::kRandomGeometric ||
           f == GraphFamily::kPoissonVoronoiApprox || f == GraphFamily::kRoadLike;
}

/// A complete description of a synthetic graph.
///
/// Every field a generator reads lives here, so a manifest that records this
/// struct records the graph. Family-specific parameters share the struct rather
/// than living in a variant: the set is small, the cost is a few unused doubles
/// per config, and a flat struct serialises to and from a manifest without a
/// discriminated-union encoding.
struct SyntheticGraphConfig {
    GraphFamily family = GraphFamily::kGrid;

    /// Requested node count. Exact for every family except kGrid and kRoadLike,
    /// which round UP to complete their lattice; see grid_rows/grid_cols.
    std::size_t node_count = 0;

    // --- kGrid, kRoadLike ----------------------------------------------------

    /// Lattice dimensions. When either is zero both are derived from
    /// `node_count` as the most nearly square lattice with at least that many
    /// nodes, and the realised node count is rows * cols, which can exceed
    /// `node_count` by up to rows - 1. Rounding up rather than down is chosen so
    /// that a requested size is never silently under-delivered, and the
    /// realised count is always readable from the built graph.
    std::size_t grid_rows = 0;
    std::size_t grid_cols = 0;

    // --- kErdosRenyi ---------------------------------------------------------

    /// Edge probability for G(n, p), in [0, 1].
    double erdos_renyi_p = 0.0;

    // --- kBarabasiAlbert -----------------------------------------------------

    /// Edges contributed by each arriving node. The seed graph is a path on
    /// m + 1 nodes, so m + 1 <= node_count is required; validate() enforces the
    /// stricter and simpler m < node_count.
    std::size_t barabasi_albert_m = 2;

    // --- kRandomGeometric ----------------------------------------------------

    /// Connection radius in the unit square, in (0, sqrt(2)].
    double geometric_radius = 0.0;

    // --- kPoissonVoronoiApprox -----------------------------------------------

    /// How many grid cells out to look for candidate Gabriel partners. Larger
    /// values cost more and miss fewer long edges; 2 is the default because the
    /// grid is sized at roughly two points per cell, so a 5x5 neighbourhood
    /// already covers many multiples of the mean nearest-neighbour distance.
    std::size_t voronoi_cell_neighbourhood = 2;

    // --- kRoadLike -----------------------------------------------------------

    /// Probability that a lattice edge outside the spanning tree is added back.
    /// Zero would leave a tree, in which every shortest-path query has a unique
    /// and trivially found answer.
    double road_extra_edge_probability = 0.15;

    /// Every road_arterial_period-th row and column is completed end to end.
    /// Zero disables arterials.
    std::size_t road_arterial_period = 8;

    /// Arterial weights are divided by this factor. Must be >= 1.
    double road_arterial_speedup = 4.0;

    /// Coordinate jitter as a fraction of the lattice spacing, in [0, 0.5).
    /// Bounded below 0.5 so jittered nodes cannot cross each other and turn the
    /// layout non-planar.
    double road_jitter = 0.25;

    // --- shared --------------------------------------------------------------

    /// Applied to kRandomGeometric and kPoissonVoronoiApprox, which are not
    /// connected in general. When set, the components are joined by adding, for
    /// each component after the first, one edge from its lowest-numbered node to
    /// the geometrically nearest node in any earlier component. This changes the
    /// family -- a connected random geometric graph is not a random geometric
    /// graph -- so it is off by default and the repair edges are ordinary edges
    /// with ordinary weights, not a hidden structure.
    bool ensure_connected = false;

    /// Weight assignment. If `weights.seed` is left at zero it is derived from
    /// `seed`, so one seed controls the whole graph; a non-zero value is used as
    /// given, which lets two graphs share a structure and differ in weights.
    WeightDistributionConfig weights;

    /// Derive weights from Euclidean edge length instead of `weights`. Only
    /// legal for families that carry coordinates. See the header comment on A*
    /// admissibility before relying on it.
    bool   euclidean_weights     = false;
    double euclidean_weight_scale = 1.0;

    Directedness directedness = Directedness::kUndirected;

    SeedType seed = 0;

    /// Refuse to generate more edges than this. The default is a memory guard,
    /// not a statement about the hardware: 5e7 undirected edges is already about
    /// 0.8 GB of arcs once both directions are materialised with 32-bit targets
    /// and 32-bit weights. Raise it deliberately for a stress run.
    std::size_t max_edges = 50'000'000;

    /// Recorded in the graph metadata. Empty means "derive one from the config".
    std::string dataset_id;

    /// Rejects configurations that cannot produce a graph, at the point the
    /// config is read rather than deep inside generation, so a bad manifest
    /// fails in the first second of a run and says why.
    void validate() const {
        weights.validate();

        if (node_count == 0) {
            throw InvalidConfigError("node_count must be positive");
        }
        if (node_count > kMaxNodeCount) {
            throw InvalidConfigError("node_count exceeds the range of NodeId");
        }
        if (max_edges == 0) {
            throw InvalidConfigError("max_edges must be positive");
        }
        if (euclidean_weights && !family_has_coordinates(family)) {
            throw InvalidConfigError(
                std::string("euclidean_weights requires a family with coordinates, but ") +
                std::string(graph_family_name(family)) + " has none");
        }
        if (euclidean_weights &&
            !(euclidean_weight_scale > 0.0 && std::isfinite(euclidean_weight_scale))) {
            throw InvalidConfigError("euclidean_weight_scale must be positive and finite");
        }

        switch (family) {
            case GraphFamily::kGrid:
            case GraphFamily::kRoadLike: {
                if ((grid_rows == 0) != (grid_cols == 0)) {
                    throw InvalidConfigError(
                        "grid_rows and grid_cols must both be set or both be left at zero");
                }
                const auto [rows, cols] = lattice_dimensions();
                if (rows > kMaxNodeCount / std::max<std::size_t>(cols, 1U)) {
                    throw InvalidConfigError("lattice dimensions overflow the node count");
                }
                const std::size_t nodes = rows * cols;
                if (nodes > kMaxNodeCount) {
                    throw InvalidConfigError("lattice node count exceeds the range of NodeId");
                }
                // A lattice has at most 2 * rows * cols edges, so this cannot
                // overflow for any node count NodeId can address.
                if (2U * nodes > max_edges) {
                    throw InvalidConfigError("lattice edge count would exceed max_edges");
                }
                if (family == GraphFamily::kRoadLike) {
                    validate_road_like();
                }
                break;
            }

            case GraphFamily::kErdosRenyi: {
                if (!(erdos_renyi_p >= 0.0 && erdos_renyi_p <= 1.0)) {
                    throw InvalidConfigError("erdos_renyi_p must lie in [0, 1]");
                }
                const double n        = static_cast<double>(node_count);
                const double expected = erdos_renyi_p * n * (n - 1.0) * 0.5;
                if (expected > static_cast<double>(max_edges)) {
                    throw InvalidConfigError(
                        "Erdos-Renyi expected edge count " +
                        std::to_string(static_cast<std::uint64_t>(expected)) +
                        " exceeds max_edges " + std::to_string(max_edges) +
                        "; lower erdos_renyi_p or raise max_edges deliberately");
                }
                break;
            }

            case GraphFamily::kBarabasiAlbert: {
                if (barabasi_albert_m == 0) {
                    throw InvalidConfigError("barabasi_albert_m must be positive");
                }
                if (barabasi_albert_m >= node_count) {
                    throw InvalidConfigError(
                        "barabasi_albert_m = " + std::to_string(barabasi_albert_m) +
                        " must be smaller than node_count = " + std::to_string(node_count) +
                        ": an arriving node cannot attach to more nodes than exist");
                }
                const double m = static_cast<double>(barabasi_albert_m);
                const double n = static_cast<double>(node_count);
                if (m * n > static_cast<double>(max_edges)) {
                    throw InvalidConfigError(
                        "Barabasi-Albert edge count would exceed max_edges");
                }
                break;
            }

            case GraphFamily::kRandomGeometric: {
                if (!(geometric_radius > 0.0) || !std::isfinite(geometric_radius)) {
                    throw InvalidConfigError("geometric_radius must be positive and finite");
                }
                if (geometric_radius > 1.4142135623730951) {
                    throw InvalidConfigError(
                        "geometric_radius exceeds the diagonal of the unit square, which would "
                        "produce the complete graph");
                }
                // Expected degree of an interior point is n * pi * r^2; the
                // boundary lowers it, so this over-estimates and therefore
                // errs towards rejecting.
                const double n = static_cast<double>(node_count);
                const double expected =
                    0.5 * n * (n - 1.0) *
                    std::min(1.0, 3.14159265358979323846 * geometric_radius * geometric_radius);
                if (expected > static_cast<double>(max_edges)) {
                    throw InvalidConfigError(
                        "random geometric expected edge count " +
                        std::to_string(static_cast<std::uint64_t>(expected)) +
                        " exceeds max_edges " + std::to_string(max_edges) +
                        "; lower geometric_radius or raise max_edges deliberately");
                }
                break;
            }

            case GraphFamily::kPoissonVoronoiApprox: {
                if (voronoi_cell_neighbourhood == 0) {
                    throw InvalidConfigError("voronoi_cell_neighbourhood must be positive");
                }
                if (voronoi_cell_neighbourhood > 64) {
                    throw InvalidConfigError(
                        "voronoi_cell_neighbourhood above 64 is quadratic in practice; if that "
                        "is really intended, use a denser point set instead");
                }
                break;
            }
        }
    }

    /// The realised lattice dimensions for kGrid and kRoadLike.
    ///
    /// Exposed because the realised node count is rows * cols, which can exceed
    /// the requested `node_count`, and a caller sizing arrays needs to know that
    /// before generating.
    [[nodiscard]] std::pair<std::size_t, std::size_t> lattice_dimensions() const noexcept {
        if (grid_rows != 0 && grid_cols != 0) {
            return {grid_rows, grid_cols};
        }
        if (node_count == 0) {
            return {0U, 0U};
        }
        auto rows = static_cast<std::size_t>(std::sqrt(static_cast<double>(node_count)));
        if (rows == 0) {
            rows = 1;
        }
        // Guard the floating-point square root: for some n, sqrt returns a value
        // whose truncation is one off. Correct it with integer arithmetic so the
        // dimensions are exactly reproducible rather than libm-dependent.
        while (rows > 1 && rows * rows > node_count) {
            --rows;
        }
        while ((rows + 1U) * (rows + 1U) <= node_count) {
            ++rows;
        }
        const std::size_t cols = (node_count + rows - 1U) / rows;
        return {rows, cols};
    }

private:
    void validate_road_like() const {
        if (!(road_extra_edge_probability >= 0.0 && road_extra_edge_probability <= 1.0)) {
            throw InvalidConfigError("road_extra_edge_probability must lie in [0, 1]");
        }
        if (!(road_arterial_speedup >= 1.0) || !std::isfinite(road_arterial_speedup)) {
            throw InvalidConfigError(
                "road_arterial_speedup must be at least 1: an arterial may not be slower than a "
                "local street");
        }
        if (!(road_jitter >= 0.0 && road_jitter < 0.5)) {
            throw InvalidConfigError(
                "road_jitter must lie in [0, 0.5): at 0.5 jittered nodes can cross and the "
                "layout stops being planar");
        }
    }
};

namespace detail {

/// A generated structure before weights are attached.
///
/// Generators return this rather than a finished edge list so that weight
/// assignment -- including the Euclidean option, the arterial speedup and the
/// quantisation -- lives in exactly one place and cannot drift between
/// families.
struct GraphSkeleton {
    std::size_t                                node_count = 0;
    std::vector<std::pair<NodeId, NodeId>>     edges;
    /// Per-edge weight multiplier, or empty when every edge is unscaled. Used
    /// only by kRoadLike's arterials.
    std::vector<double>                        multipliers;
    std::vector<Coordinate>                    coordinates;
    CoordinateSystem                           coord_system = CoordinateSystem::kNone;
};

/// Union-find over node ids, used to find and repair disconnected components.
///
/// Path halving plus union by size: near-constant amortised cost, and iterative,
/// so a path-shaped component cannot overflow the stack the way a recursive find
/// would on a million-node lattice.
class DisjointSets {
public:
    explicit DisjointSets(std::size_t n) : parent_(n), size_(n, 1U) {
        for (std::size_t i = 0; i < n; ++i) {
            parent_[i] = static_cast<NodeId>(i);
        }
    }

    [[nodiscard]] NodeId find(NodeId x) noexcept {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x          = parent_[x];
        }
        return x;
    }

    bool unite(NodeId a, NodeId b) noexcept {
        NodeId ra = find(a);
        NodeId rb = find(b);
        if (ra == rb) {
            return false;
        }
        if (size_[ra] < size_[rb]) {
            std::swap(ra, rb);
        }
        parent_[rb] = ra;
        size_[ra] += size_[rb];
        return true;
    }

private:
    std::vector<NodeId>      parent_;
    std::vector<std::size_t> size_;
};

/// A uniform bucket grid over the unit square.
///
/// Both spatial families need "which points are near this point", and a uniform
/// grid is the right structure here rather than a k-d tree: the points are
/// uniform by construction, so the buckets are balanced, and a grid lookup is a
/// couple of integer operations with no pointer chasing.
class PointGrid {
public:
    PointGrid(const std::vector<Coordinate>& points, std::size_t side)
        : side_(std::max<std::size_t>(side, 1U)), cells_(side_ * side_) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            cells_[cell_index(points[i])].push_back(static_cast<NodeId>(i));
        }
    }

    [[nodiscard]] std::size_t side() const noexcept { return side_; }

    [[nodiscard]] std::size_t cell_of_axis(double v) const noexcept {
        const double scaled = v * static_cast<double>(side_);
        if (!(scaled > 0.0)) {
            return 0U;
        }
        const auto idx = static_cast<std::size_t>(scaled);
        return std::min(idx, side_ - 1U);
    }

    [[nodiscard]] std::size_t cell_index(const Coordinate& c) const noexcept {
        return cell_of_axis(c.y) * side_ + cell_of_axis(c.x);
    }

    [[nodiscard]] const std::vector<NodeId>& cell(std::size_t cx, std::size_t cy) const noexcept {
        return cells_[cy * side_ + cx];
    }

private:
    std::size_t                      side_;
    std::vector<std::vector<NodeId>> cells_;
};

[[nodiscard]] inline double squared_distance(const Coordinate& a, const Coordinate& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/// Uniform points in the unit square, drawn from the supplied Rng in node order
/// so the sample is a pure function of the seed.
[[nodiscard]] inline std::vector<Coordinate> sample_unit_square(std::size_t n, Rng& rng) {
    std::vector<Coordinate> pts(n);
    for (std::size_t i = 0; i < n; ++i) {
        pts[i].x = rng.next_unit();
        pts[i].y = rng.next_unit();
    }
    return pts;
}

/// Adds one edge per extra connected component, joining it to the nearest node
/// in any earlier component.
///
/// "Earlier" means "belonging to a component whose lowest node id is smaller",
/// which is a deterministic ordering independent of the traversal. The nearest
/// partner is found by a full scan over the nodes already joined; that is
/// O(components * n), which is acceptable precisely because a configuration
/// worth repairing has few components. A configuration with many components is
/// a configuration whose radius is too small, and that is better fixed in the
/// config than hidden here.
inline void connect_components(GraphSkeleton& sk) {
    if (sk.node_count < 2U || sk.coordinates.size() != sk.node_count) {
        return;
    }
    DisjointSets ds(sk.node_count);
    for (const auto& e : sk.edges) {
        (void)ds.unite(e.first, e.second);
    }

    // `joined` accumulates the nodes of every component processed so far.
    std::vector<NodeId> joined;
    joined.reserve(sk.node_count);
    std::vector<char> component_seen(sk.node_count, 0);

    for (std::size_t i = 0; i < sk.node_count; ++i) {
        const auto   u    = static_cast<NodeId>(i);
        const NodeId root = ds.find(u);
        if (component_seen[root] == 0) {
            component_seen[root] = 1;
            if (!joined.empty()) {
                // First node of a new component: link it to the nearest node
                // seen so far, which by construction lies in another component.
                NodeId best      = joined.front();
                double best_dist = squared_distance(sk.coordinates[u], sk.coordinates[best]);
                for (const NodeId v : joined) {
                    const double d = squared_distance(sk.coordinates[u], sk.coordinates[v]);
                    if (d < best_dist) {
                        best_dist = d;
                        best      = v;
                    }
                }
                sk.edges.emplace_back(u, best);
                if (!sk.multipliers.empty()) {
                    sk.multipliers.push_back(1.0);
                }
                (void)ds.unite(u, best);
            }
        }
        joined.push_back(u);
    }
}

// -----------------------------------------------------------------------------
// Family generators
// -----------------------------------------------------------------------------

[[nodiscard]] inline GraphSkeleton generate_grid(const SyntheticGraphConfig& cfg) {
    const auto [rows, cols] = cfg.lattice_dimensions();

    GraphSkeleton sk;
    sk.node_count   = rows * cols;
    sk.coord_system = CoordinateSystem::kEuclidean;
    sk.coordinates.resize(sk.node_count);
    sk.edges.reserve(rows * (cols - (cols > 0 ? 1U : 0U)) + cols * (rows > 0 ? rows - 1U : 0U));

    const auto id = [cols](std::size_t r, std::size_t c) {
        return static_cast<NodeId>(r * cols + c);
    };
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            sk.coordinates[id(r, c)] =
                Coordinate{static_cast<double>(c), static_cast<double>(r)};
            if (c + 1U < cols) {
                sk.edges.emplace_back(id(r, c), id(r, c + 1U));
            }
            if (r + 1U < rows) {
                sk.edges.emplace_back(id(r, c), id(r + 1U, c));
            }
        }
    }
    return sk;
}

[[nodiscard]] inline GraphSkeleton generate_erdos_renyi(const SyntheticGraphConfig& cfg,
                                                        Rng&                        rng) {
    GraphSkeleton sk;
    sk.node_count = cfg.node_count;

    const double p = cfg.erdos_renyi_p;
    if (!(p > 0.0)) {
        return sk;
    }

    // Batagelj-Brandes: the gap between consecutive accepted pairs in the
    // lexicographic enumeration of {(v, w) : 0 <= w < v < n} is geometric with
    // parameter p, so it can be sampled directly. The loop therefore runs once
    // per EMITTED edge, not once per candidate pair.
    const double log_q = std::log1p(-p);  // negative; -inf exactly when p == 1.

    std::int64_t       v = 1;
    std::int64_t       w = -1;
    const auto         n = static_cast<std::int64_t>(cfg.node_count);
    const std::size_t  cap = cfg.max_edges;

    while (v < n) {
        double r = rng.next_unit();
        // next_unit() returns [0, 1), so log1p(-r) is finite and <= 0.
        const double skip = std::floor(std::log1p(-r) / log_q);
        // p == 1 makes log_q == -inf and skip == -0.0, i.e. every pair is taken.
        w += 1 + static_cast<std::int64_t>(skip);
        while (w >= v && v < n) {
            w -= v;
            ++v;
        }
        if (v < n) {
            if (sk.edges.size() >= cap) {
                throw InvalidConfigError(
                    "Erdos-Renyi generation exceeded max_edges = " + std::to_string(cap) +
                    "; the realised edge count ran past the configured ceiling");
            }
            sk.edges.emplace_back(static_cast<NodeId>(v), static_cast<NodeId>(w));
        }
    }
    return sk;
}

[[nodiscard]] inline GraphSkeleton generate_barabasi_albert(const SyntheticGraphConfig& cfg,
                                                            Rng&                        rng) {
    const std::size_t m = cfg.barabasi_albert_m;

    GraphSkeleton sk;
    sk.node_count = cfg.node_count;
    sk.edges.reserve(m * cfg.node_count);

    // The seed graph is a path on m + 1 nodes rather than an empty or complete
    // graph on m nodes. A path is connected, which makes the whole result
    // connected by induction (every arriving node attaches to an existing one),
    // and it supplies a non-empty attachment list without the degree bias a
    // complete seed clique would bake in.
    const std::size_t seed_nodes = m + 1U;

    /// Each node appears once per incident edge endpoint, so drawing uniformly
    /// from this list is exactly drawing a node with probability proportional to
    /// its degree. This is the standard O(1)-per-draw preferential attachment;
    /// the alternative, a prefix-sum search over degrees, is O(log n) per draw
    /// and buys nothing here.
    std::vector<NodeId> repeated;
    repeated.reserve(2U * m * cfg.node_count);

    for (std::size_t i = 0; i + 1U < seed_nodes; ++i) {
        const auto a = static_cast<NodeId>(i);
        const auto b = static_cast<NodeId>(i + 1U);
        sk.edges.emplace_back(a, b);
        repeated.push_back(a);
        repeated.push_back(b);
    }

    std::vector<NodeId> chosen;
    chosen.reserve(m);

    for (std::size_t v = seed_nodes; v < cfg.node_count; ++v) {
        chosen.clear();
        const auto source = static_cast<NodeId>(v);

        while (chosen.size() < m) {
            const std::size_t idx =
                static_cast<std::size_t>(rng.next_below(static_cast<std::uint64_t>(
                    repeated.size())));
            const NodeId candidate = repeated[idx];
            if (std::find(chosen.begin(), chosen.end(), candidate) == chosen.end()) {
                chosen.push_back(candidate);
                continue;
            }
            // The draw collided with an already-chosen target. Rather than
            // retrying indefinitely -- which can be very slow once m approaches
            // the number of distinct nodes in the list -- walk forward from the
            // drawn position to the next usable node. The walk starts at a
            // uniformly drawn offset, so it stays deterministic and unbiased
            // enough for a structural generator; it is not claimed to preserve
            // the exact preferential-attachment law in that collision case.
            bool found = false;
            for (std::size_t step = 1; step <= repeated.size(); ++step) {
                const NodeId alt = repeated[(idx + step) % repeated.size()];
                if (std::find(chosen.begin(), chosen.end(), alt) == chosen.end()) {
                    chosen.push_back(alt);
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Cannot happen: validate() guarantees m < node_count and the
                // list always holds at least m distinct nodes by this point.
                throw InternalError("Barabasi-Albert attachment list ran out of distinct nodes");
            }
        }

        for (const NodeId t : chosen) {
            sk.edges.emplace_back(source, t);
            repeated.push_back(source);
            repeated.push_back(t);
        }
    }
    return sk;
}

[[nodiscard]] inline GraphSkeleton generate_random_geometric(const SyntheticGraphConfig& cfg,
                                                             Rng&                        rng) {
    GraphSkeleton sk;
    sk.node_count   = cfg.node_count;
    sk.coord_system = CoordinateSystem::kEuclidean;
    sk.coordinates  = sample_unit_square(cfg.node_count, rng);

    const double r  = cfg.geometric_radius;
    const double r2 = r * r;

    // Cells are at least r across, so every point within r of a given point lies
    // in the 3x3 neighbourhood of that point's cell. The search is therefore
    // EXACT, not a heuristic. The side count is also capped near sqrt(n) so that
    // a tiny radius cannot allocate an enormous, almost entirely empty grid.
    const auto        by_radius = static_cast<std::size_t>(std::floor(1.0 / r));
    const auto        by_points =
        static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(cfg.node_count)))) + 1U;
    const std::size_t side = std::max<std::size_t>(1U, std::min(by_radius, by_points));

    const PointGrid grid(sk.coordinates, side);

    for (std::size_t cy = 0; cy < grid.side(); ++cy) {
        for (std::size_t cx = 0; cx < grid.side(); ++cx) {
            for (const NodeId u : grid.cell(cx, cy)) {
                const std::size_t x_lo = cx > 0 ? cx - 1U : 0U;
                const std::size_t y_lo = cy > 0 ? cy - 1U : 0U;
                const std::size_t x_hi = std::min(cx + 1U, grid.side() - 1U);
                const std::size_t y_hi = std::min(cy + 1U, grid.side() - 1U);
                for (std::size_t ny = y_lo; ny <= y_hi; ++ny) {
                    for (std::size_t nx = x_lo; nx <= x_hi; ++nx) {
                        for (const NodeId v : grid.cell(nx, ny)) {
                            // u < v emits each unordered pair exactly once.
                            if (v <= u) {
                                continue;
                            }
                            if (squared_distance(sk.coordinates[u], sk.coordinates[v]) <= r2) {
                                if (sk.edges.size() >= cfg.max_edges) {
                                    throw InvalidConfigError(
                                        "random geometric generation exceeded max_edges = " +
                                        std::to_string(cfg.max_edges));
                                }
                                sk.edges.emplace_back(u, v);
                            }
                        }
                    }
                }
            }
        }
    }

    if (cfg.ensure_connected) {
        connect_components(sk);
    }
    return sk;
}

[[nodiscard]] inline GraphSkeleton generate_poisson_voronoi_approx(
    const SyntheticGraphConfig& cfg, Rng& rng) {
    GraphSkeleton sk;
    sk.node_count   = cfg.node_count;
    sk.coord_system = CoordinateSystem::kEuclidean;
    sk.coordinates  = sample_unit_square(cfg.node_count, rng);

    // Roughly two points per cell: dense enough that a bounded cell
    // neighbourhood covers several mean nearest-neighbour distances, sparse
    // enough that the per-cell scans stay short.
    const auto        target_cells = std::max<std::size_t>(1U, cfg.node_count / 2U);
    const std::size_t side =
        std::max<std::size_t>(1U, static_cast<std::size_t>(
                                      std::sqrt(static_cast<double>(target_cells))));
    const PointGrid grid(sk.coordinates, side);
    const auto      k = cfg.voronoi_cell_neighbourhood;

    // The Gabriel predicate, evaluated exactly: {u, v} is an edge when no other
    // point lies strictly inside the disk with uv as its diameter. Every cell
    // the disk touches is scanned, so this cannot report a false edge. Only the
    // CANDIDATE set is restricted; see the header comment.
    const auto gabriel = [&](NodeId u, NodeId v) {
        const Coordinate& a = sk.coordinates[u];
        const Coordinate& b = sk.coordinates[v];
        const Coordinate  centre{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
        const double      radius2 = 0.25 * squared_distance(a, b);
        const double      radius  = std::sqrt(radius2);
        // A strict interior test with a relative slack, so that a third point
        // sitting exactly on the circle -- which is the degenerate cocircular
        // case -- is treated as outside and the edge is kept. Dropping it
        // instead would make the result depend on rounding.
        const double tolerance = radius2 * 1e-12;

        const std::size_t x_lo = grid.cell_of_axis(centre.x - radius);
        const std::size_t x_hi = grid.cell_of_axis(centre.x + radius);
        const std::size_t y_lo = grid.cell_of_axis(centre.y - radius);
        const std::size_t y_hi = grid.cell_of_axis(centre.y + radius);

        for (std::size_t cy = y_lo; cy <= y_hi; ++cy) {
            for (std::size_t cx = x_lo; cx <= x_hi; ++cx) {
                for (const NodeId z : grid.cell(cx, cy)) {
                    if (z == u || z == v) {
                        continue;
                    }
                    if (squared_distance(sk.coordinates[z], centre) < radius2 - tolerance) {
                        return false;
                    }
                }
            }
        }
        return true;
    };

    for (std::size_t cy = 0; cy < grid.side(); ++cy) {
        for (std::size_t cx = 0; cx < grid.side(); ++cx) {
            for (const NodeId u : grid.cell(cx, cy)) {
                const std::size_t x_lo = cx > k ? cx - k : 0U;
                const std::size_t y_lo = cy > k ? cy - k : 0U;
                const std::size_t x_hi = std::min(cx + k, grid.side() - 1U);
                const std::size_t y_hi = std::min(cy + k, grid.side() - 1U);
                for (std::size_t ny = y_lo; ny <= y_hi; ++ny) {
                    for (std::size_t nx = x_lo; nx <= x_hi; ++nx) {
                        for (const NodeId v : grid.cell(nx, ny)) {
                            if (v <= u) {
                                continue;
                            }
                            if (!gabriel(u, v)) {
                                continue;
                            }
                            if (sk.edges.size() >= cfg.max_edges) {
                                throw InvalidConfigError(
                                    "Poisson Voronoi generation exceeded max_edges = " +
                                    std::to_string(cfg.max_edges));
                            }
                            sk.edges.emplace_back(u, v);
                        }
                    }
                }
            }
        }
    }

    if (cfg.ensure_connected) {
        connect_components(sk);
    }
    return sk;
}

[[nodiscard]] inline GraphSkeleton generate_road_like(const SyntheticGraphConfig& cfg, Rng& rng) {
    const auto [rows, cols] = cfg.lattice_dimensions();

    GraphSkeleton sk;
    sk.node_count   = rows * cols;
    sk.coord_system = CoordinateSystem::kEuclidean;
    sk.coordinates.resize(sk.node_count);

    const auto id = [cols](std::size_t r, std::size_t c) {
        return static_cast<NodeId>(r * cols + c);
    };

    // Jittered positions. The jitter is drawn before any structural decision so
    // that the layout depends only on the seed and the dimensions, not on the
    // spanning tree.
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const double jx = cfg.road_jitter > 0.0
                                  ? rng.next_double(-cfg.road_jitter, cfg.road_jitter)
                                  : 0.0;
            const double jy = cfg.road_jitter > 0.0
                                  ? rng.next_double(-cfg.road_jitter, cfg.road_jitter)
                                  : 0.0;
            sk.coordinates[id(r, c)] =
                Coordinate{static_cast<double>(c) + jx, static_cast<double>(r) + jy};
        }
    }

    // Lattice edges are addressed by a dense index so membership is a bit test
    // rather than a hash lookup: horizontals first, then verticals.
    const std::size_t horizontals = cols > 0 ? rows * (cols - 1U) : 0U;
    const std::size_t verticals   = rows > 0 ? (rows - 1U) * cols : 0U;
    std::vector<char> included(horizontals + verticals, 0);

    const auto horizontal_index = [cols](std::size_t r, std::size_t c) {
        return r * (cols - 1U) + c;
    };
    const auto vertical_index = [cols, horizontals](std::size_t r, std::size_t c) {
        return horizontals + r * cols + c;
    };

    // A uniformly random spanning tree of the lattice by randomised depth-first
    // search. This is what makes the result connected BY CONSTRUCTION rather
    // than with high probability, which matters: a benchmark graph that is
    // occasionally disconnected produces unreachable queries and silently
    // changes what is being measured.
    if (sk.node_count > 0) {
        std::vector<char>        visited(sk.node_count, 0);
        std::vector<std::size_t> stack;
        stack.reserve(sk.node_count);
        stack.push_back(0U);
        visited[0] = 1;

        while (!stack.empty()) {
            const std::size_t cur = stack.back();
            const std::size_t r   = cur / cols;
            const std::size_t c   = cur % cols;

            // Unvisited lattice neighbours, in a shuffled order.
            std::vector<int> dirs = {0, 1, 2, 3};  // left, right, up, down
            rng.shuffle(dirs);

            bool advanced = false;
            for (const int d : dirs) {
                std::size_t nr = r;
                std::size_t nc = c;
                if (d == 0) {
                    if (c == 0) continue;
                    nc = c - 1U;
                } else if (d == 1) {
                    if (c + 1U >= cols) continue;
                    nc = c + 1U;
                } else if (d == 2) {
                    if (r == 0) continue;
                    nr = r - 1U;
                } else {
                    if (r + 1U >= rows) continue;
                    nr = r + 1U;
                }
                const std::size_t next = nr * cols + nc;
                if (visited[next] != 0) {
                    continue;
                }
                visited[next] = 1;
                if (nr == r) {
                    included[horizontal_index(r, std::min(c, nc))] = 1;
                } else {
                    included[vertical_index(std::min(r, nr), c)] = 1;
                }
                stack.push_back(next);
                advanced = true;
                break;
            }
            if (!advanced) {
                stack.pop_back();
            }
        }
    }

    // Extra edges, so the graph has cycles and shortest paths are not unique by
    // construction.
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c + 1U < cols; ++c) {
            const std::size_t e = horizontal_index(r, c);
            if (included[e] == 0 && rng.next_unit() < cfg.road_extra_edge_probability) {
                included[e] = 1;
            }
        }
    }
    for (std::size_t r = 0; r + 1U < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t e = vertical_index(r, c);
            if (included[e] == 0 && rng.next_unit() < cfg.road_extra_edge_probability) {
                included[e] = 1;
            }
        }
    }

    // Arterials: complete rows and columns, marked so their weights are divided
    // by the speedup factor. This is the hierarchy that makes a road network
    // respond to A* and to contraction-style techniques.
    std::vector<char> arterial(included.size(), 0);
    if (cfg.road_arterial_period > 0) {
        for (std::size_t r = 0; r < rows; r += cfg.road_arterial_period) {
            for (std::size_t c = 0; c + 1U < cols; ++c) {
                const std::size_t e = horizontal_index(r, c);
                included[e]         = 1;
                arterial[e]         = 1;
            }
        }
        for (std::size_t c = 0; c < cols; c += cfg.road_arterial_period) {
            for (std::size_t r = 0; r + 1U < rows; ++r) {
                const std::size_t e = vertical_index(r, c);
                included[e]         = 1;
                arterial[e]         = 1;
            }
        }
    }

    sk.multipliers.reserve(included.size());
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c + 1U < cols; ++c) {
            const std::size_t e = horizontal_index(r, c);
            if (included[e] != 0) {
                sk.edges.emplace_back(id(r, c), id(r, c + 1U));
                sk.multipliers.push_back(arterial[e] != 0 ? 1.0 / cfg.road_arterial_speedup
                                                          : 1.0);
            }
        }
    }
    for (std::size_t r = 0; r + 1U < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t e = vertical_index(r, c);
            if (included[e] != 0) {
                sk.edges.emplace_back(id(r, c), id(r + 1U, c));
                sk.multipliers.push_back(arterial[e] != 0 ? 1.0 / cfg.road_arterial_speedup
                                                          : 1.0);
            }
        }
    }
    return sk;
}

}  // namespace detail

/// The weight configuration a config actually generates with.
///
/// Exposed so a caller can record in a manifest the seed that was really used,
/// rather than the zero placeholder that means "derive it".
[[nodiscard]] inline WeightDistributionConfig effective_weight_config(
    const SyntheticGraphConfig& cfg) {
    WeightDistributionConfig w = cfg.weights;
    if (w.seed == 0) {
        // Derived rather than reused verbatim so that the structure draws and
        // the weight hashes do not share a seed; a shared seed is harmless here
        // but makes two independent sources look correlated in a manifest.
        w.seed = detail::mix64(cfg.seed ^ 0xA5A5A5A5A5A5A5A5ULL);
        if (w.seed == 0) {
            w.seed = 1;
        }
    }
    return w;
}

/// A dataset identifier derived from the config, used when none was supplied.
///
/// Deliberately not a hash: a human reading a results file should be able to see
/// which family and size produced a row without consulting a lookup table.
[[nodiscard]] inline std::string default_dataset_id(const SyntheticGraphConfig& cfg) {
    std::string id = "synthetic:";
    id += graph_family_name(cfg.family);
    id += ":n=" + std::to_string(cfg.node_count);
    id += ":w=" + std::string(weight_distribution_name(cfg.weights.kind));
    id += ":seed=" + std::to_string(cfg.seed);
    return id;
}

/// Generates the graph described by `cfg`.
///
/// The single entry point for every family. Validation happens first and
/// throws InvalidConfigError; structural generation, then weight assignment,
/// then CSR construction through GraphBuilder, which is the only sanctioned way
/// to produce a CsrGraph and applies its own invariant checks.
template <WeightType W>
[[nodiscard]] CsrGraph<W> generate_graph(const SyntheticGraphConfig& cfg) {
    cfg.validate();

    Rng rng(cfg.seed);

    detail::GraphSkeleton sk;
    switch (cfg.family) {
        case GraphFamily::kGrid:
            sk = detail::generate_grid(cfg);
            break;
        case GraphFamily::kErdosRenyi:
            sk = detail::generate_erdos_renyi(cfg, rng);
            break;
        case GraphFamily::kBarabasiAlbert:
            sk = detail::generate_barabasi_albert(cfg, rng);
            break;
        case GraphFamily::kRandomGeometric:
            sk = detail::generate_random_geometric(cfg, rng);
            break;
        case GraphFamily::kPoissonVoronoiApprox:
            sk = detail::generate_poisson_voronoi_approx(cfg, rng);
            break;
        case GraphFamily::kRoadLike:
            sk = detail::generate_road_like(cfg, rng);
            break;
    }

    if (sk.edges.size() > cfg.max_edges) {
        throw InvalidConfigError("generated edge count " + std::to_string(sk.edges.size()) +
                                 " exceeds max_edges " + std::to_string(cfg.max_edges));
    }

    const WeightDistributionConfig wcfg = effective_weight_config(cfg);

    std::vector<InputEdge<W>> edges;
    // A directed request materialises both arcs here rather than in the builder,
    // because the builder pairs arcs only for graphs labelled undirected.
    const bool directed = cfg.directedness == Directedness::kDirected;
    edges.reserve(sk.edges.size() * (directed ? 2U : 1U));

    for (std::size_t i = 0; i < sk.edges.size(); ++i) {
        const auto [u, v] = sk.edges[i];

        double value = 0.0;
        if (cfg.euclidean_weights) {
            const double dx = sk.coordinates[u].x - sk.coordinates[v].x;
            const double dy = sk.coordinates[u].y - sk.coordinates[v].y;
            value           = std::hypot(dx, dy) * cfg.euclidean_weight_scale;
        } else {
            value = sample_weight(wcfg, u, v);
        }
        if (!sk.multipliers.empty()) {
            value *= sk.multipliers[i];
        }
        const W w = quantize_weight<W>(value, wcfg);

        edges.push_back(InputEdge<W>{u, v, w});
        if (directed && u != v) {
            edges.push_back(InputEdge<W>{v, u, w});
        }
    }

    GraphBuildOptions opts;
    opts.directedness = cfg.directedness;
    // Both directions are already present when the caller asked for a directed
    // graph; for an undirected one each edge appears once and the builder pairs
    // it. Either way parallel edges and self-loops are impossible by
    // construction, so the permissive policies never fire.
    opts.arcs_already_paired = directed;
    opts.parallel_edges      = ParallelEdgePolicy::kKeep;
    opts.self_loops          = SelfLoopPolicy::kKeep;
    opts.coord_system        = sk.coordinates.empty() ? CoordinateSystem::kNone : sk.coord_system;
    opts.dataset_id          = cfg.dataset_id.empty() ? default_dataset_id(cfg) : cfg.dataset_id;

    return GraphBuilder<W>::build(sk.node_count, std::move(edges), opts,
                                  std::move(sk.coordinates));
}

}  // namespace lpq

#endif  // LPQ_GRAPH_SYNTHETIC_GRAPHS_HPP
