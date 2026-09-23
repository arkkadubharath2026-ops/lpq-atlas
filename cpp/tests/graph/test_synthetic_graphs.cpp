// =============================================================================
// Tests for the synthetic graph generator (PRD.md section 17, ESD.md section 28).
//
// What is tested here is what the rest of the measurement system relies on,
// which is narrower than "these are correct random graph models":
//
//   * determinism -- a config plus a seed identifies a graph exactly, because
//     an experiment manifest records the config and nothing else. A generator
//     that drifted between runs would invalidate every comparison built on it.
//   * structural invariants -- each family really has the shape its name
//     claims, because the families exist precisely to separate queues that a
//     single shape would not separate. A grid that was quietly not a grid would
//     make a whole row of results meaningless without failing anything.
//   * symmetry -- undirected families produce paired arcs. This is the property
//     the weight hashing in weight_distributions.hpp exists to protect, so it
//     is checked on the generated graphs and not only on the weight sampler.
//   * weight bounds and coordinate presence -- the two contracts downstream
//     code reads without re-checking: a bucket queue sizes itself from the
//     weight range, and A* dereferences coordinates.
//   * config validation -- the impossible cases must be rejected when the
//     config is read, not after an hour of generation.
//
// The Gabriel predicate of kPoissonVoronoiApprox is verified exhaustively on a
// small instance, against every point rather than against the grid-local
// candidates, because that predicate is the only part of the family that could
// produce a WRONG edge as opposed to a missing one. Missing edges are a
// documented limitation of the candidate restriction and are not asserted
// against.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/synthetic_graphs.hpp"
#include "lpq/graph/weight_distributions.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;
using Graph  = CsrGraph<Weight>;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

/// A config whose weights are bounded and whose seed is fixed, so that every
/// test below is replayable exactly.
SyntheticGraphConfig base_config(GraphFamily family, std::size_t nodes, SeedType seed = 1234) {
    SyntheticGraphConfig cfg;
    cfg.family          = family;
    cfg.node_count      = nodes;
    cfg.seed            = seed;
    cfg.weights.kind    = WeightDistribution::kUniform;
    cfg.weights.minimum = 1.0;
    cfg.weights.maximum = 1000.0;
    return cfg;
}

/// Exact structural and weight equality. Compares arcs rather than only counts,
/// because two different graphs can easily share both counts.
[[nodiscard]] bool graphs_identical(const Graph& a, const Graph& b) {
    if (a.num_nodes() != b.num_nodes() || a.num_arcs() != b.num_arcs()) {
        return false;
    }
    for (NodeId u = 0; u < a.num_nodes(); ++u) {
        const auto lhs = a.out_arcs(u);
        const auto rhs = b.out_arcs(u);
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (!(lhs[i] == rhs[i])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::size_t component_count(const Graph& g) {
    std::vector<char> seen(g.num_nodes(), 0);
    std::size_t       components = 0;
    for (NodeId s = 0; s < g.num_nodes(); ++s) {
        if (seen[s] != 0) {
            continue;
        }
        ++components;
        seen[s] = 1;
        std::queue<NodeId> frontier;
        frontier.push(s);
        while (!frontier.empty()) {
            const NodeId u = frontier.front();
            frontier.pop();
            for (const auto& arc : g.out_arcs(u)) {
                if (seen[arc.target] == 0) {
                    seen[arc.target] = 1;
                    frontier.push(arc.target);
                }
            }
        }
    }
    return components;
}

/// Every arc has a reverse arc of the same weight.
[[nodiscard]] bool adjacency_is_symmetric(const Graph& g) {
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            const auto back = g.out_arcs(arc.target);
            const bool paired =
                std::any_of(back.begin(), back.end(), [&](const Arc<Weight>& r) {
                    return r.target == u && r.weight == arc.weight;
                });
            if (!paired) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::size_t> degrees(const Graph& g) {
    std::vector<std::size_t> d(g.num_nodes());
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        d[u] = g.out_arcs(u).size();
    }
    return d;
}

[[nodiscard]] double euclidean(const Coordinate& a, const Coordinate& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// -----------------------------------------------------------------------------
// Names and parsing
// -----------------------------------------------------------------------------

TEST(SyntheticGraphFamily, NamesRoundTrip) {
    const std::vector<GraphFamily> families = {
        GraphFamily::kGrid,           GraphFamily::kErdosRenyi,
        GraphFamily::kBarabasiAlbert, GraphFamily::kRandomGeometric,
        GraphFamily::kPoissonVoronoiApprox, GraphFamily::kRoadLike,
    };
    for (const GraphFamily f : families) {
        GraphFamily parsed = GraphFamily::kGrid;
        ASSERT_TRUE(parse_graph_family(graph_family_name(f), parsed))
            << graph_family_name(f);
        EXPECT_EQ(parsed, f);
    }
}

TEST(SyntheticGraphFamily, UnknownNameIsRejectedRatherThanDefaulted) {
    GraphFamily parsed = GraphFamily::kRoadLike;
    EXPECT_FALSE(parse_graph_family("gird", parsed));
    EXPECT_FALSE(parse_graph_family("", parsed));
    EXPECT_FALSE(parse_graph_family("Grid", parsed));
    // The out parameter is untouched on failure, so a caller that ignores the
    // return value at least keeps its own default rather than acquiring one.
    EXPECT_EQ(parsed, GraphFamily::kRoadLike);
}

TEST(SyntheticGraphFamily, VoronoiAliasMapsToTheAccuratelyNamedFamily) {
    GraphFamily parsed = GraphFamily::kGrid;
    ASSERT_TRUE(parse_graph_family("poisson_voronoi", parsed));
    EXPECT_EQ(parsed, GraphFamily::kPoissonVoronoiApprox);
    // The canonical name is the accurate one, so that is what a manifest records.
    EXPECT_EQ(graph_family_name(parsed), "poisson_voronoi_approx");
}

TEST(SyntheticGraphFamily, CoordinateFamiliesAreDeclaredConsistently) {
    EXPECT_TRUE(family_has_coordinates(GraphFamily::kGrid));
    EXPECT_TRUE(family_has_coordinates(GraphFamily::kRandomGeometric));
    EXPECT_TRUE(family_has_coordinates(GraphFamily::kPoissonVoronoiApprox));
    EXPECT_TRUE(family_has_coordinates(GraphFamily::kRoadLike));
    EXPECT_FALSE(family_has_coordinates(GraphFamily::kErdosRenyi));
    EXPECT_FALSE(family_has_coordinates(GraphFamily::kBarabasiAlbert));
}

// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------

class SyntheticGraphFamilies : public testing::TestWithParam<GraphFamily> {
protected:
    static SyntheticGraphConfig config_for(GraphFamily family, std::size_t nodes,
                                           SeedType seed) {
        SyntheticGraphConfig cfg = base_config(family, nodes, seed);
        cfg.erdos_renyi_p        = 0.01;
        cfg.barabasi_albert_m    = 3;
        cfg.geometric_radius     = 0.06;
        return cfg;
    }
};

TEST_P(SyntheticGraphFamilies, SameSeedProducesAnIdenticalGraph) {
    const auto cfg = config_for(GetParam(), 1000, 20240517);
    const auto a   = generate_graph<Weight>(cfg);
    const auto b   = generate_graph<Weight>(cfg);

    EXPECT_EQ(a.num_nodes(), b.num_nodes());
    EXPECT_EQ(a.num_arcs(), b.num_arcs());
    EXPECT_TRUE(graphs_identical(a, b));
}

TEST_P(SyntheticGraphFamilies, DifferentSeedProducesADifferentGraph) {
    const auto a = generate_graph<Weight>(config_for(GetParam(), 1000, 1));
    const auto b = generate_graph<Weight>(config_for(GetParam(), 1000, 2));

    // The node count is a function of the config, not the seed, so only the
    // structure or the weights may differ -- and for every family here at least
    // one of them must.
    EXPECT_EQ(a.num_nodes(), b.num_nodes());
    EXPECT_FALSE(graphs_identical(a, b));
}

TEST_P(SyntheticGraphFamilies, UndirectedFamiliesHaveSymmetricAdjacency) {
    const auto cfg = config_for(GetParam(), 800, 99);
    const auto g   = generate_graph<Weight>(cfg);
    ASSERT_EQ(g.directedness(), Directedness::kUndirected);
    EXPECT_TRUE(adjacency_is_symmetric(g));
    // num_edges() halves the arc count for an undirected graph, so an odd arc
    // count would mean an unpaired arc slipped through.
    EXPECT_EQ(g.num_arcs() % 2U, 0U);
}

TEST_P(SyntheticGraphFamilies, CoordinatesArePresentExactlyForSpatialFamilies) {
    const auto cfg = config_for(GetParam(), 600, 77);
    const auto g   = generate_graph<Weight>(cfg);

    if (family_has_coordinates(GetParam())) {
        ASSERT_TRUE(g.has_coordinates());
        EXPECT_EQ(g.coordinate_system(), CoordinateSystem::kEuclidean);
        EXPECT_EQ(g.coordinates().size(), g.num_nodes());
    } else {
        EXPECT_FALSE(g.has_coordinates());
        EXPECT_EQ(g.coordinate_system(), CoordinateSystem::kNone);
        EXPECT_TRUE(g.coordinates().empty());
    }
}

TEST_P(SyntheticGraphFamilies, NoSelfLoopsAndNoParallelArcs) {
    const auto cfg = config_for(GetParam(), 700, 5);
    const auto g   = generate_graph<Weight>(cfg);
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        const auto arcs = g.out_arcs(u);
        for (std::size_t i = 0; i < arcs.size(); ++i) {
            EXPECT_NE(arcs[i].target, u) << "self-loop at " << u;
            if (i + 1U < arcs.size()) {
                // Adjacency is sorted by (target, weight) by the builder, so
                // duplicates would be adjacent.
                EXPECT_NE(arcs[i].target, arcs[i + 1U].target) << "parallel arc at " << u;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllFamilies, SyntheticGraphFamilies,
                         testing::Values(GraphFamily::kGrid, GraphFamily::kErdosRenyi,
                                         GraphFamily::kBarabasiAlbert,
                                         GraphFamily::kRandomGeometric,
                                         GraphFamily::kPoissonVoronoiApprox,
                                         GraphFamily::kRoadLike),
                         // The parameter is deliberately not named `info`: the
                         // INSTANTIATE_TEST_SUITE_P macro expands this lambda
                         // inside a function that already has a parameter of
                         // that name, and -Wshadow is an error in this build.
                         [](const testing::TestParamInfo<GraphFamily>& param_info) {
                             return std::string(graph_family_name(param_info.param));
                         });

// -----------------------------------------------------------------------------
// Grid
// -----------------------------------------------------------------------------

TEST(SyntheticGrid, ExplicitDimensionsGiveTheExactLattice) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 1);
    cfg.grid_rows            = 7;
    cfg.grid_cols            = 11;

    const auto g = generate_graph<Weight>(cfg);
    EXPECT_EQ(g.num_nodes(), 7U * 11U);
    // A rows x cols lattice has rows*(cols-1) horizontal and (rows-1)*cols
    // vertical edges.
    EXPECT_EQ(g.num_edges(), 7U * 10U + 6U * 11U);
}

TEST(SyntheticGrid, BoundaryDegreesAreTwoThreeAndFour) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 1);
    cfg.grid_rows            = 5;
    cfg.grid_cols            = 6;
    const auto g             = generate_graph<Weight>(cfg);

    std::size_t corners = 0;
    std::size_t sides   = 0;
    std::size_t inner   = 0;
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t c = 0; c < 6; ++c) {
            const auto        u        = static_cast<NodeId>(r * 6U + c);
            const bool        r_edge   = (r == 0 || r == 4);
            const bool        c_edge   = (c == 0 || c == 5);
            const std::size_t expected = 4U - (r_edge ? 1U : 0U) - (c_edge ? 1U : 0U);
            EXPECT_EQ(g.out_arcs(u).size(), expected) << "at (" << r << "," << c << ")";
            if (expected == 2U) {
                ++corners;
            } else if (expected == 3U) {
                ++sides;
            } else {
                ++inner;
            }
        }
    }
    EXPECT_EQ(corners, 4U);
    EXPECT_EQ(sides, 2U * (5U - 2U) + 2U * (6U - 2U));
    EXPECT_EQ(inner, 3U * 4U);
}

TEST(SyntheticGrid, CoordinatesMatchLatticePositions) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 1);
    cfg.grid_rows            = 4;
    cfg.grid_cols            = 4;
    const auto g             = generate_graph<Weight>(cfg);

    ASSERT_TRUE(g.has_coordinates());
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            const auto&      xy = g.coordinate(static_cast<NodeId>(r * 4U + c));
            EXPECT_DOUBLE_EQ(xy.x, static_cast<double>(c));
            EXPECT_DOUBLE_EQ(xy.y, static_cast<double>(r));
        }
    }
    // Every lattice edge joins nodes exactly one unit apart, which is what makes
    // the Euclidean and Manhattan heuristics meaningful on this family.
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            EXPECT_DOUBLE_EQ(euclidean(g.coordinate(u), g.coordinate(arc.target)), 1.0);
        }
    }
}

TEST(SyntheticGrid, DerivedDimensionsRoundUpAndAreReportedByTheConfig) {
    // 1000 is not a perfect square, so the lattice is completed upwards: the
    // realised node count must be at least the requested one, and must equal
    // the dimensions the config reports.
    const SyntheticGraphConfig cfg    = base_config(GraphFamily::kGrid, 1000);
    const auto [rows, cols]           = cfg.lattice_dimensions();
    const auto                 g      = generate_graph<Weight>(cfg);

    EXPECT_EQ(g.num_nodes(), rows * cols);
    EXPECT_GE(g.num_nodes(), cfg.node_count);
    EXPECT_LT(g.num_nodes(), cfg.node_count + rows);
}

TEST(SyntheticGrid, IsConnected) {
    const auto g = generate_graph<Weight>(base_config(GraphFamily::kGrid, 400));
    EXPECT_EQ(component_count(g), 1U);
}

// -----------------------------------------------------------------------------
// Erdos-Renyi
// -----------------------------------------------------------------------------

TEST(SyntheticErdosRenyi, EdgeCountIsNearTheExpectation) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 2000);
    cfg.erdos_renyi_p        = 0.005;
    const auto g             = generate_graph<Weight>(cfg);

    EXPECT_EQ(g.num_nodes(), 2000U);
    const double n        = 2000.0;
    const double expected = cfg.erdos_renyi_p * n * (n - 1.0) * 0.5;
    const double sd       = std::sqrt(expected * (1.0 - cfg.erdos_renyi_p));
    // Six standard deviations: a fixed seed makes this deterministic, and the
    // band is wide enough that it is testing the generator rather than the draw.
    EXPECT_NEAR(static_cast<double>(g.num_edges()), expected, 6.0 * sd);
}

TEST(SyntheticErdosRenyi, ProbabilityOneGivesTheCompleteGraph) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 40);
    cfg.erdos_renyi_p        = 1.0;
    const auto g             = generate_graph<Weight>(cfg);

    EXPECT_EQ(g.num_edges(), 40U * 39U / 2U);
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        EXPECT_EQ(g.out_arcs(u).size(), 39U);
    }
}

TEST(SyntheticErdosRenyi, ProbabilityZeroGivesAnEmptyEdgeSet) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 50);
    cfg.erdos_renyi_p        = 0.0;
    const auto g             = generate_graph<Weight>(cfg);

    EXPECT_EQ(g.num_nodes(), 50U);
    EXPECT_EQ(g.num_arcs(), 0U);
}

// -----------------------------------------------------------------------------
// Barabasi-Albert
// -----------------------------------------------------------------------------

TEST(SyntheticBarabasiAlbert, HasTheEdgeCountItsConstructionImplies) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 2000);
    cfg.barabasi_albert_m    = 3;
    const auto g             = generate_graph<Weight>(cfg);

    // Seed path on m+1 nodes contributes m edges; each of the remaining
    // n - (m+1) nodes contributes m.
    const std::size_t m        = cfg.barabasi_albert_m;
    const std::size_t expected = m + (2000U - (m + 1U)) * m;
    EXPECT_EQ(g.num_edges(), expected);
}

TEST(SyntheticBarabasiAlbert, IsConnectedByConstruction) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 3000);
    cfg.barabasi_albert_m    = 2;
    const auto g             = generate_graph<Weight>(cfg);
    EXPECT_EQ(component_count(g), 1U);
    // No node may be isolated: every arriving node attaches to m existing ones.
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        EXPECT_GE(g.out_arcs(u).size(), 1U) << "isolated node " << u;
    }
}

TEST(SyntheticBarabasiAlbert, DegreeDistributionIsHeavyTailed) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 5000);
    cfg.barabasi_albert_m    = 2;
    const auto g             = generate_graph<Weight>(cfg);

    auto d = degrees(g);
    ASSERT_FALSE(d.empty());
    const std::size_t max_degree = *std::max_element(d.begin(), d.end());
    std::sort(d.begin(), d.end());
    const std::size_t median = d[d.size() / 2U];
    const double      mean   = static_cast<double>(g.num_arcs()) /
                        static_cast<double>(g.num_nodes());

    // The signature of a power law, stated as an inequality rather than as a
    // fitted exponent: the bulk sits at the attachment degree while the maximum
    // is an order of magnitude above the mean. A fitted exponent would be
    // testing an estimator, not the generator.
    EXPECT_LE(median, cfg.barabasi_albert_m + 2U);
    EXPECT_GT(static_cast<double>(max_degree), 10.0 * mean);
}

// -----------------------------------------------------------------------------
// Random geometric
// -----------------------------------------------------------------------------

TEST(SyntheticRandomGeometric, EveryEdgeIsWithinTheRadius) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRandomGeometric, 400);
    cfg.geometric_radius     = 0.1;
    const auto g             = generate_graph<Weight>(cfg);

    ASSERT_TRUE(g.has_coordinates());
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            EXPECT_LE(euclidean(g.coordinate(u), g.coordinate(arc.target)),
                      cfg.geometric_radius);
        }
    }
}

TEST(SyntheticRandomGeometric, NoPairWithinTheRadiusIsMissed) {
    // The grid search is exact, not approximate, so the complement holds too:
    // brute force over all pairs must find the same edge set.
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRandomGeometric, 300);
    cfg.geometric_radius     = 0.12;
    const auto g             = generate_graph<Weight>(cfg);

    std::size_t expected_edges = 0;
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (NodeId v = static_cast<NodeId>(u + 1U); v < g.num_nodes(); ++v) {
            if (euclidean(g.coordinate(u), g.coordinate(v)) <= cfg.geometric_radius) {
                ++expected_edges;
                const auto arcs = g.out_arcs(u);
                EXPECT_TRUE(std::any_of(arcs.begin(), arcs.end(),
                                        [v](const Arc<Weight>& a) { return a.target == v; }))
                    << "missing edge " << u << "-" << v;
            }
        }
    }
    EXPECT_EQ(g.num_edges(), expected_edges);
}

TEST(SyntheticRandomGeometric, CoordinatesLieInTheUnitSquare) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRandomGeometric, 500);
    cfg.geometric_radius     = 0.05;
    const auto g             = generate_graph<Weight>(cfg);
    for (const Coordinate& c : g.coordinates()) {
        EXPECT_GE(c.x, 0.0);
        EXPECT_LT(c.x, 1.0);
        EXPECT_GE(c.y, 0.0);
        EXPECT_LT(c.y, 1.0);
    }
}

TEST(SyntheticRandomGeometric, EnsureConnectedJoinsTheComponents) {
    // A radius well below the connectivity threshold, so the unrepaired graph
    // is genuinely fragmented and the repair has something to do.
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRandomGeometric, 400, 31337);
    cfg.geometric_radius     = 0.03;

    const auto fragmented = generate_graph<Weight>(cfg);
    ASSERT_GT(component_count(fragmented), 1U);

    cfg.ensure_connected  = true;
    const auto repaired   = generate_graph<Weight>(cfg);
    EXPECT_EQ(component_count(repaired), 1U);
    EXPECT_GT(repaired.num_edges(), fragmented.num_edges());
    EXPECT_TRUE(adjacency_is_symmetric(repaired));
}

// -----------------------------------------------------------------------------
// Poisson Voronoi (Gabriel) approximation
// -----------------------------------------------------------------------------

TEST(SyntheticPoissonVoronoi, EveryReportedEdgeSatisfiesTheGabrielPredicate) {
    // Checked against EVERY point, not against the grid-local candidate set, so
    // this test would catch a bug in the spatial index as well as in the
    // predicate. It says nothing about missing edges, which the candidate
    // restriction is documented to allow.
    const auto cfg = base_config(GraphFamily::kPoissonVoronoiApprox, 250, 6161);
    const auto g   = generate_graph<Weight>(cfg);

    ASSERT_TRUE(g.has_coordinates());
    ASSERT_GT(g.num_arcs(), 0U);

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            const NodeId     v = arc.target;
            const Coordinate a = g.coordinate(u);
            const Coordinate b = g.coordinate(v);
            const Coordinate centre{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
            const double     radius = 0.5 * euclidean(a, b);

            for (NodeId z = 0; z < g.num_nodes(); ++z) {
                if (z == u || z == v) {
                    continue;
                }
                // Strictly inside, with the same relative slack the generator
                // uses for the cocircular case.
                EXPECT_GE(euclidean(g.coordinate(z), centre), radius * (1.0 - 1e-9))
                    << "point " << z << " lies inside the diametral disk of " << u << "-" << v;
            }
        }
    }
}

TEST(SyntheticPoissonVoronoi, IsSparseAndPlanarLikeRatherThanDense) {
    const auto g = generate_graph<Weight>(base_config(GraphFamily::kPoissonVoronoiApprox, 4000));

    const double mean_degree =
        static_cast<double>(g.num_arcs()) / static_cast<double>(g.num_nodes());
    // A planar graph has fewer than 3n edges, so its mean degree is below 6; the
    // Gabriel graph is a strict subgraph of the Delaunay triangulation and is
    // sparser still. The lower bound is there so that a generator that silently
    // produced almost nothing would fail rather than pass a one-sided check.
    EXPECT_GT(mean_degree, 2.0);
    EXPECT_LT(mean_degree, 6.0);
    EXPECT_LE(g.num_edges(), 3U * g.num_nodes());
}

// -----------------------------------------------------------------------------
// Road-like
// -----------------------------------------------------------------------------

TEST(SyntheticRoadLike, IsConnectedByConstruction) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRoadLike, 2500);
    cfg.road_extra_edge_probability = 0.0;
    cfg.road_arterial_period        = 0;
    // Even with no extra edges and no arterials, the spanning tree alone must
    // connect the lattice.
    const auto g = generate_graph<Weight>(cfg);
    EXPECT_EQ(component_count(g), 1U);
    EXPECT_EQ(g.num_edges(), g.num_nodes() - 1U) << "a spanning tree has exactly n-1 edges";
}

TEST(SyntheticRoadLike, ExtraEdgesAndArterialsOnlyAddEdges) {
    SyntheticGraphConfig tree    = base_config(GraphFamily::kRoadLike, 2500);
    tree.road_extra_edge_probability = 0.0;
    tree.road_arterial_period        = 0;

    SyntheticGraphConfig full = tree;
    full.road_extra_edge_probability = 0.2;
    full.road_arterial_period        = 8;

    const auto a = generate_graph<Weight>(tree);
    const auto b = generate_graph<Weight>(full);

    EXPECT_EQ(a.num_nodes(), b.num_nodes());
    EXPECT_GT(b.num_edges(), a.num_edges());
    EXPECT_EQ(component_count(b), 1U);
}

TEST(SyntheticRoadLike, DegreeStaysBoundedByTheLattice) {
    const auto g = generate_graph<Weight>(base_config(GraphFamily::kRoadLike, 1600));
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        EXPECT_LE(g.out_arcs(u).size(), 4U);
    }
}

TEST(SyntheticRoadLike, ArterialsAreFasterThanLocalStreets) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRoadLike, 2500);
    cfg.weights.kind         = WeightDistribution::kConstant;
    cfg.weights.minimum      = 100.0;
    cfg.weights.maximum      = 100.0;
    cfg.road_arterial_period = 8;
    cfg.road_arterial_speedup = 4.0;

    const auto g = generate_graph<Weight>(cfg);

    // With a constant base weight, an arc is either a local street at 100 or an
    // arterial at 100/4. Both must occur, and nothing else may.
    std::size_t local    = 0;
    std::size_t arterial = 0;
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            if (arc.weight == 100U) {
                ++local;
            } else if (arc.weight == 25U) {
                ++arterial;
            } else {
                ADD_FAILURE() << "unexpected weight " << arc.weight;
            }
        }
    }
    EXPECT_GT(local, 0U);
    EXPECT_GT(arterial, 0U);
}

TEST(SyntheticRoadLike, JitterStaysWithinItsBound) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRoadLike, 900);
    cfg.road_jitter          = 0.25;
    const auto [rows, cols]  = cfg.lattice_dimensions();
    const auto g             = generate_graph<Weight>(cfg);

    ASSERT_TRUE(g.has_coordinates());
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const auto& xy = g.coordinate(static_cast<NodeId>(r * cols + c));
            EXPECT_LT(std::fabs(xy.x - static_cast<double>(c)), cfg.road_jitter);
            EXPECT_LT(std::fabs(xy.y - static_cast<double>(r)), cfg.road_jitter);
        }
    }
}

// -----------------------------------------------------------------------------
// Weights
// -----------------------------------------------------------------------------

TEST(SyntheticGraphWeights, EveryDistributionRespectsItsBounds) {
    const std::vector<WeightDistribution> kinds = {
        WeightDistribution::kConstant,    WeightDistribution::kUniform,
        WeightDistribution::kLogNormal,   WeightDistribution::kExponential,
        WeightDistribution::kNarrowRange, WeightDistribution::kHeavyTailed,
    };

    for (const WeightDistribution kind : kinds) {
        SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 900);
        cfg.weights.kind         = kind;
        cfg.weights.minimum      = 5.0;
        cfg.weights.maximum      = 500.0;

        const auto g = generate_graph<Weight>(cfg);
        ASSERT_GT(g.num_arcs(), 0U) << weight_distribution_name(kind);
        for (NodeId u = 0; u < g.num_nodes(); ++u) {
            for (const auto& arc : g.out_arcs(u)) {
                EXPECT_GE(arc.weight, 5U) << weight_distribution_name(kind);
                EXPECT_LE(arc.weight, 500U) << weight_distribution_name(kind);
            }
        }
    }
}

TEST(SyntheticGraphWeights, StructureIsIndependentOfTheWeightSeed) {
    // Two graphs that share a structural seed but not a weight seed must have
    // the same arcs and different weights. This is what lets an experiment vary
    // the weight distribution while holding the topology fixed.
    SyntheticGraphConfig a = base_config(GraphFamily::kRandomGeometric, 500);
    a.geometric_radius     = 0.08;
    a.weights.seed         = 11;

    SyntheticGraphConfig b = a;
    b.weights.seed         = 12;

    const auto ga = generate_graph<Weight>(a);
    const auto gb = generate_graph<Weight>(b);

    ASSERT_EQ(ga.num_nodes(), gb.num_nodes());
    ASSERT_EQ(ga.num_arcs(), gb.num_arcs());

    for (NodeId u = 0; u < ga.num_nodes(); ++u) {
        const auto x = ga.out_arcs(u);
        const auto y = gb.out_arcs(u);
        ASSERT_EQ(x.size(), y.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            EXPECT_EQ(x[i].target, y[i].target);
        }
    }
    EXPECT_FALSE(graphs_identical(ga, gb)) << "weight seed had no effect";
}

TEST(SyntheticGraphWeights, EuclideanWeightsTrackEdgeLength) {
    SyntheticGraphConfig cfg    = base_config(GraphFamily::kRandomGeometric, 400);
    cfg.geometric_radius        = 0.08;
    cfg.euclidean_weights       = true;
    cfg.euclidean_weight_scale  = 1000.0;

    const auto g = generate_graph<double>(cfg);
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const auto& arc : g.out_arcs(u)) {
            const double expected =
                euclidean(g.coordinate(u), g.coordinate(arc.target)) * 1000.0;
            EXPECT_NEAR(arc.weight, expected, 1e-9);
        }
    }
}

TEST(SyntheticGraphWeights, EffectiveWeightSeedIsDerivedWhenLeftAtZero) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 100, 4242);
    ASSERT_EQ(cfg.weights.seed, 0U);
    EXPECT_NE(effective_weight_config(cfg).seed, 0U);

    cfg.weights.seed = 98765;
    EXPECT_EQ(effective_weight_config(cfg).seed, 98765U);
}

TEST(SyntheticGraphWeights, DatasetIdRecordsFamilyAndSize) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 128, 9);
    cfg.barabasi_albert_m    = 2;
    const auto g             = generate_graph<Weight>(cfg);
    EXPECT_NE(g.dataset_id().find("barabasi_albert"), std::string::npos);
    EXPECT_NE(g.dataset_id().find("128"), std::string::npos);

    cfg.dataset_id = "explicit-id";
    EXPECT_EQ(generate_graph<Weight>(cfg).dataset_id(), "explicit-id");
}

// -----------------------------------------------------------------------------
// Directedness
// -----------------------------------------------------------------------------

TEST(SyntheticGraphDirectedness, DirectedRequestMaterialisesBothArcs) {
    SyntheticGraphConfig undirected = base_config(GraphFamily::kGrid, 100);
    SyntheticGraphConfig directed   = undirected;
    directed.directedness           = Directedness::kDirected;

    const auto u = generate_graph<Weight>(undirected);
    const auto d = generate_graph<Weight>(directed);

    EXPECT_EQ(u.directedness(), Directedness::kUndirected);
    EXPECT_EQ(d.directedness(), Directedness::kDirected);
    // Same arcs either way; only the label and hence num_edges() differ.
    EXPECT_EQ(u.num_arcs(), d.num_arcs());
    EXPECT_EQ(d.num_edges(), d.num_arcs());
    EXPECT_EQ(u.num_edges(), u.num_arcs() / 2U);
    EXPECT_TRUE(adjacency_is_symmetric(d));
}

// -----------------------------------------------------------------------------
// Configuration validation
// -----------------------------------------------------------------------------

TEST(SyntheticGraphConfigValidation, RejectsZeroNodes) {
    const SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 0);
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
    EXPECT_THROW({ [[maybe_unused]] const auto g = generate_graph<Weight>(cfg); },
                 InvalidConfigError);
}

TEST(SyntheticGraphConfigValidation, RejectsBarabasiAlbertWithMAtLeastN) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 10);
    cfg.barabasi_albert_m    = 10;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.barabasi_albert_m = 11;
    EXPECT_THROW({ [[maybe_unused]] const auto g = generate_graph<Weight>(cfg); },
                 InvalidConfigError);

    cfg.barabasi_albert_m = 0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    // The largest legal value must still work, so the bound is exclusive rather
    // than accidentally off by one in the other direction.
    cfg.barabasi_albert_m = 9;
    EXPECT_NO_THROW({ [[maybe_unused]] const auto g = generate_graph<Weight>(cfg); });
}

TEST(SyntheticGraphConfigValidation, RejectsProbabilityOutsideTheUnitInterval) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 100);
    cfg.erdos_renyi_p        = 1.5;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.erdos_renyi_p = -0.1;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
}

TEST(SyntheticGraphConfigValidation, RejectsErdosRenyiThatWouldExhaustMemory) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 1'000'000);
    cfg.erdos_renyi_p        = 0.5;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    // The same n is fine once p makes the graph sparse.
    cfg.erdos_renyi_p = 1e-6;
    EXPECT_NO_THROW({ cfg.validate(); });
}

TEST(SyntheticGraphConfigValidation, RejectsNonPositiveOrOversizedGeometricRadius) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRandomGeometric, 100);
    cfg.geometric_radius     = 0.0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.geometric_radius = 2.0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.geometric_radius = 0.3;
    cfg.max_edges        = 10;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
}

TEST(SyntheticGraphConfigValidation, RejectsEuclideanWeightsOnFamiliesWithoutCoordinates) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kErdosRenyi, 100);
    cfg.erdos_renyi_p        = 0.01;
    cfg.euclidean_weights    = true;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.family           = GraphFamily::kGrid;
    EXPECT_NO_THROW({ cfg.validate(); });

    cfg.euclidean_weight_scale = 0.0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
}

TEST(SyntheticGraphConfigValidation, RejectsPartiallySpecifiedLatticeDimensions) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 100);
    cfg.grid_rows            = 10;
    cfg.grid_cols            = 0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.grid_cols = 10;
    EXPECT_NO_THROW({ cfg.validate(); });
}

TEST(SyntheticGraphConfigValidation, RejectsImpossibleRoadLikeParameters) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kRoadLike, 400);

    cfg.road_extra_edge_probability = 1.5;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
    cfg.road_extra_edge_probability = 0.1;

    cfg.road_arterial_speedup = 0.5;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
    cfg.road_arterial_speedup = 2.0;

    cfg.road_jitter = 0.5;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
    cfg.road_jitter = 0.49;

    EXPECT_NO_THROW({ cfg.validate(); });
}

TEST(SyntheticGraphConfigValidation, RejectsInvalidWeightConfiguration) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kGrid, 100);
    cfg.weights.minimum      = 10.0;
    cfg.weights.maximum      = 1.0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
}

TEST(SyntheticGraphConfigValidation, RejectsZeroMaxEdgesAndAnUnusableVoronoiNeighbourhood) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kPoissonVoronoiApprox, 100);
    cfg.max_edges            = 0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.max_edges                  = 1000;
    cfg.voronoi_cell_neighbourhood = 0;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);

    cfg.voronoi_cell_neighbourhood = 1000;
    EXPECT_THROW({ cfg.validate(); }, InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Small and degenerate sizes
// -----------------------------------------------------------------------------

TEST(SyntheticGraphEdgeCases, SingleNodeGraphsAreValidAndEdgeless) {
    for (const GraphFamily family :
         {GraphFamily::kGrid, GraphFamily::kErdosRenyi, GraphFamily::kRandomGeometric,
          GraphFamily::kPoissonVoronoiApprox, GraphFamily::kRoadLike}) {
        SyntheticGraphConfig cfg = base_config(family, 1);
        cfg.erdos_renyi_p        = 0.5;
        cfg.geometric_radius     = 0.5;

        const auto g = generate_graph<Weight>(cfg);
        EXPECT_EQ(g.num_nodes(), 1U) << graph_family_name(family);
        EXPECT_EQ(g.num_arcs(), 0U) << graph_family_name(family);
    }
}

TEST(SyntheticGraphEdgeCases, TwoNodeBarabasiAlbertIsASingleEdge) {
    SyntheticGraphConfig cfg = base_config(GraphFamily::kBarabasiAlbert, 2);
    cfg.barabasi_albert_m    = 1;
    const auto g             = generate_graph<Weight>(cfg);
    EXPECT_EQ(g.num_nodes(), 2U);
    EXPECT_EQ(g.num_edges(), 1U);
}

}  // namespace
}  // namespace lpq
