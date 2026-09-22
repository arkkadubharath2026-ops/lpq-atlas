// =============================================================================
// A* tests (ESD.md sections 9, 24).
//
// The governing invariant from ESD.md section 24 is "admissible heuristic
// preserves optimality", so the central assertion throughout is that A* returns
// the same path COSTS as Dijkstra. Which path it records may differ, since
// equally optimal paths are broken differently by a different key order.
//
// A suite that only asserted "A* agrees with Dijkstra" could pass while being
// blind to the failure it is meant to catch, so it also contains the converse:
// a deliberately inadmissible heuristic, asserted to produce a genuinely
// suboptimal path. Without that, an A* that silently ignored its heuristic
// would pass every other test here.
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "lpq/algorithms/astar.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/heuristics.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/radix_heap.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using lpq::astar;
using lpq::AstarOptions;
using lpq::CoordinateSystem;
using lpq::EuclideanHeuristic;
using lpq::HaversineHeuristic;
using lpq::kInvalidNode;
using lpq::ManhattanHeuristic;
using lpq::NodeId;
using lpq::reference_dijkstra;
using lpq::SearchTermination;
using lpq::ZeroHeuristic;

using W     = std::uint32_t;
using Dist  = lpq::DistanceType<W>;
using Graph = lpq::CsrGraph<W>;

using Binary = lpq::BinaryHeap<Dist>;

// --- The governing invariant --------------------------------------------------

TEST(Astar, ZeroHeuristicReproducesDijkstraExactly) {
    // The control for every other A* experiment: with h == 0, A* *is* Dijkstra,
    // so any later speedup can be attributed to the heuristic rather than to
    // some difference in the A* implementation.
    const auto g = lpq_test::grid_graph<W>(15, 15);

    for (NodeId target : {NodeId{0}, NodeId{7}, NodeId{112}, NodeId{224}}) {
        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, 0, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;
        const auto got = astar<W, Binary, ZeroHeuristic<W>>(g, 0, aopts);

        ASSERT_EQ(got.termination, SearchTermination::kTargetReached) << "target " << target;
        EXPECT_EQ(got.dist[target], expected.dist[target]) << "target " << target;
    }
}

TEST(Astar, EuclideanPreservesOptimalityOnAGrid) {
    const auto g = lpq_test::grid_graph<W>(25, 25);

    for (NodeId target = 0; target < 625; target += 37) {
        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, 0, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;
        const auto got = astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts);

        EXPECT_EQ(got.dist[target], expected.dist[target]) << "target " << target;
    }
}

TEST(Astar, ManhattanPreservesOptimalityOnAGrid) {
    // Manhattan is exact on a 4-connected unit grid: the L1 distance IS the
    // shortest-path cost, so this is the tightest admissible heuristic here.
    const auto g = lpq_test::grid_graph<W>(25, 25);

    for (NodeId target = 0; target < 625; target += 41) {
        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, 0, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;
        const auto got = astar<W, Binary, ManhattanHeuristic<W>>(g, 0, aopts);

        EXPECT_EQ(got.dist[target], expected.dist[target]) << "target " << target;
    }
}

// --- The heuristic must actually do something ---------------------------------

TEST(Astar, AGoodHeuristicSettlesFewerNodesThanDijkstra) {
    // Optimality alone does not prove the heuristic is being used: an A* that
    // ignored h entirely would pass every optimality test above. Manhattan is
    // exact on this grid, so it should expand dramatically fewer nodes.
    //
    // The evidence is a counter, and counters compile out entirely when
    // instrumentation is disabled (ESD.md section 19), so in that build the
    // property is simply not observable. Skipping is honest; asserting 0 < 0
    // would be a false failure, and relaxing the assertion would silently
    // retire a real check.
    if constexpr (!lpq::kInstrumentationEnabled) {
        GTEST_SKIP() << "settled-node counts are compiled out in this build";
    }

    const auto g      = lpq_test::grid_graph<W>(40, 40);
    const NodeId target = 40 * 40 - 1;  // opposite corner

    AstarOptions aopts;
    aopts.target       = target;
    aopts.measure_time = false;

    const auto with_zero      = astar<W, Binary, ZeroHeuristic<W>>(g, 0, aopts);
    const auto with_manhattan = astar<W, Binary, ManhattanHeuristic<W>>(g, 0, aopts);

    EXPECT_EQ(with_zero.dist[target], with_manhattan.dist[target])
        << "both must find the optimal cost";
    EXPECT_LT(with_manhattan.counters.settled_nodes, with_zero.counters.settled_nodes)
        << "an informative heuristic must reduce the explored frontier";
}

// --- The converse: the suite can detect a broken heuristic --------------------

/// A heuristic read from an explicit per-node table.
///
/// Lets a test state exactly what h is at every node, rather than hoping a
/// geometric formula produces the intended shape. It claims consistency so it
/// can be paired with any queue; for the table used below that claim is false,
/// which is precisely the condition under test.
template <lpq::WeightType Weight>
class TableHeuristic {
public:
    static constexpr bool kIsConsistent = true;  // deliberately untrue below

    explicit TableHeuristic(std::vector<lpq::DistanceType<Weight>> table)
        : table_(std::move(table)) {}

    void prepare(const Graph&, NodeId) noexcept {}

    [[nodiscard]] lpq::DistanceType<Weight> estimate(NodeId v) const noexcept {
        return v < table_.size() ? table_[v] : lpq::DistanceType<Weight>{};
    }

    [[nodiscard]] std::string name() const { return "table"; }

private:
    std::vector<lpq::DistanceType<Weight>> table_;
};

TEST(Astar, AnInadmissibleHeuristicProducesSuboptimalPaths) {
    // The converse check. If an inadmissible heuristic could never make A*
    // return a suboptimal path, the optimality tests above would prove nothing
    // -- an A* that ignored h entirely would pass all of them.
    //
    // The mechanism requires the target to be EXTRACTED before the cheaper
    // route is completed, which needs a heuristic that overestimates on the
    // cheap route specifically. An earlier attempt used a uniformly scaled
    // geometric heuristic on a grid with an express lane; that made A* slower
    // but still optimal, because f fell steeply along the cheap lane and those
    // nodes were explored anyway. A uniform overestimate is therefore not
    // sufficient, and the graph is now built to isolate the failure:
    //
    //        0 ---------- 100 ----------> 2        (direct, expensive)
    //        0 --10--> 1 --10--> 2                 (via mid, cheap: 20)
    //
    // with h(1) = 1000, an overestimate on the cheap route only.
    //
    // A* pops 0, relaxes both: node 2 gets f = 100 + 0 = 100, node 1 gets
    // f = 10 + 1000 = 1010. Node 2 is extracted at f = 100 and A* stops,
    // reporting 100 while the optimum is 20.
    std::vector<lpq::InputEdge<W>> edges{
        {0, 2, 100},  // direct, expensive
        {0, 1, 10},   // cheap route, first hop
        {1, 2, 10},   // cheap route, second hop
    };
    lpq::GraphBuildOptions bopts;
    bopts.dataset_id = "fixture:inadmissible-trap";
    const auto g = lpq::GraphBuilder<W>::build(3, std::move(edges), bopts);

    lpq::ReferenceDijkstraOptions ropts;
    ropts.target = 2;
    const auto optimal = reference_dijkstra(g, 0, ropts);
    ASSERT_EQ(optimal.dist[2], 20U) << "test premise: the cheap route costs 20";

    AstarOptions aopts;
    aopts.target       = 2;
    aopts.measure_time = false;

    // Admissible control: h == 0 everywhere. Must stay optimal.
    {
        TableHeuristic<W> zero({0, 0, 0});
        Binary            q;
        q.reserve_nodes(g.num_nodes());
        const auto r = astar<W, Binary, TableHeuristic<W>>(g, 0, q, zero, aopts);
        EXPECT_EQ(r.dist[2], optimal.dist[2]) << "an admissible heuristic must stay optimal";
    }

    // Inadmissible: overestimates only on the cheap route.
    {
        TableHeuristic<W> bad({0, 1000, 0});
        Binary            q;
        q.reserve_nodes(g.num_nodes());
        const auto r = astar<W, Binary, TableHeuristic<W>>(g, 0, q, bad, aopts);
        EXPECT_GT(r.dist[2], optimal.dist[2])
            << "an inadmissible heuristic must be able to return a costlier path; "
               "if it cannot, the optimality tests above are not exercising h at all";
        EXPECT_EQ(r.dist[2], 100U) << "it should return the expensive direct arc";
    }
}

// --- Every queue --------------------------------------------------------------

TEST(Astar, AllQueuesAgreeWithTheReference) {
    const auto g = lpq_test::grid_graph<W>(20, 20);

    for (NodeId target = 0; target < 400; target += 53) {
        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, 0, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;

        const auto ctx = " target " + std::to_string(target);

        EXPECT_EQ((astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts)).dist[target],
                  expected.dist[target]) << "binary_heap" << ctx;
        EXPECT_EQ((astar<W, lpq::FourAryHeap<Dist>, EuclideanHeuristic<W>>(g, 0, aopts))
                      .dist[target],
                  expected.dist[target]) << "four_ary_heap" << ctx;
        EXPECT_EQ((astar<W, lpq::PairingHeap<Dist>, EuclideanHeuristic<W>>(g, 0, aopts))
                      .dist[target],
                  expected.dist[target]) << "pairing_heap" << ctx;
        EXPECT_EQ((astar<W, lpq::FibonacciHeap<Dist>, EuclideanHeuristic<W>>(g, 0, aopts))
                      .dist[target],
                  expected.dist[target]) << "fibonacci_heap" << ctx;
        // Legal because every shipped heuristic is consistent, which is what
        // the radix heap's monotone-key precondition requires.
        EXPECT_EQ((astar<W, lpq::RadixHeap<Dist>, ManhattanHeuristic<W>>(g, 0, aopts))
                      .dist[target],
                  expected.dist[target]) << "radix_heap" << ctx;
    }
}

// --- Path validity ------------------------------------------------------------

TEST(Astar, RecordedPathIsValidAndCostsWhatIsReported) {
    const auto   g      = lpq_test::grid_graph<W>(18, 18);
    const NodeId target = 18 * 18 - 1;

    AstarOptions aopts;
    aopts.target       = target;
    aopts.measure_time = false;
    const auto r = astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts);

    const auto path = r.path_to(target);
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(path.front(), 0U);
    EXPECT_EQ(path.back(), target);
    // Recomputed from the graph, so this does not merely trust dist[].
    EXPECT_EQ(r.verify_path_cost(g, path), r.dist[target]);
}

TEST(Astar, DistArrayHoldsGNotF) {
    // A classic bug: storing the key f in dist[] makes the reported cost
    // include the heuristic term. The source's cost must be exactly zero even
    // though its key is h(source), which is large here.
    const auto   g      = lpq_test::grid_graph<W>(15, 15);
    const NodeId target = 15 * 15 - 1;

    AstarOptions aopts;
    aopts.target       = target;
    aopts.measure_time = false;
    const auto r = astar<W, Binary, ManhattanHeuristic<W>>(g, 0, aopts);

    EXPECT_EQ(r.dist[0], 0U) << "dist[] must hold g, not f";
    EXPECT_EQ(r.dist[target], 28U) << "14 right + 14 down on a unit grid";
}

// --- Degenerate cases ---------------------------------------------------------

TEST(Astar, SourceEqualsTarget) {
    const auto   g = lpq_test::grid_graph<W>(5, 5);
    AstarOptions aopts;
    aopts.target       = 0;
    aopts.measure_time = false;

    const auto r = astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts);
    EXPECT_EQ(r.termination, SearchTermination::kTargetReached);
    EXPECT_EQ(r.dist[0], 0U);
}

TEST(Astar, UnreachableTarget) {
    // Two disjoint grid components sharing one coordinate space.
    std::vector<lpq::InputEdge<W>> edges{{0, 1, 1}, {2, 3, 1}};
    std::vector<lpq::Coordinate>   coords{{0, 0}, {1, 0}, {10, 10}, {11, 10}};
    lpq::GraphBuildOptions         bopts;
    bopts.coord_system = CoordinateSystem::kEuclidean;
    bopts.dataset_id   = "fixture:split";
    const auto g = lpq::GraphBuilder<W>::build(4, std::move(edges), bopts, std::move(coords));

    AstarOptions aopts;
    aopts.target       = 3;
    aopts.measure_time = false;
    const auto r = astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts);

    EXPECT_EQ(r.termination, SearchTermination::kTargetUnreachable);
    EXPECT_FALSE(r.reachable(3));
}

TEST(Astar, RequiresATarget) {
    const auto g = lpq_test::grid_graph<W>(4, 4);
    EXPECT_THROW((void)(astar<W, Binary, ZeroHeuristic<W>>(g, 0, AstarOptions{})),
                 lpq::InvalidConfigError);
}

TEST(Astar, OutOfRangeTargetIsHandled) {
    const auto   g = lpq_test::grid_graph<W>(4, 4);
    AstarOptions aopts;
    aopts.target       = 9999;
    aopts.measure_time = false;
    const auto r = astar<W, Binary, ZeroHeuristic<W>>(g, 0, aopts);
    EXPECT_EQ(r.termination, SearchTermination::kTargetUnreachable);
}

TEST(Astar, GeometricHeuristicRejectsAGraphWithoutCoordinates) {
    const auto   g = lpq_test::diamond_graph();  // no coordinates
    AstarOptions aopts;
    aopts.target       = 3;
    aopts.measure_time = false;
    EXPECT_THROW((void)(astar<W, Binary, EuclideanHeuristic<W>>(g, 0, aopts)),
                 lpq::UnsupportedConfigurationError);
}

// --- Heuristic units ----------------------------------------------------------

TEST(Heuristics, EstimatesAreFlooredNotRounded) {
    // Rounding to nearest can push the estimate above the true cost and destroy
    // admissibility; flooring cannot.
    const auto g = lpq_test::grid_graph<W>(3, 3);
    EuclideanHeuristic<W> h(1.0);
    h.prepare(g, 8);  // corner (2,2)

    // Node 0 is at (0,0): true Euclidean distance is sqrt(8) ~ 2.828.
    EXPECT_EQ(h.estimate(0), 2U) << "must floor 2.828, not round to 3";
}

TEST(Heuristics, ZeroHeuristicIsAlwaysZero) {
    ZeroHeuristic<W> h;
    EXPECT_EQ(h.estimate(0), 0U);
    EXPECT_EQ(h.estimate(12345), 0U);
}

TEST(Heuristics, RejectNonPositiveScale) {
    EXPECT_THROW(EuclideanHeuristic<W>(0.0), lpq::InvalidConfigError);
    EXPECT_THROW(EuclideanHeuristic<W>(-1.0), lpq::InvalidConfigError);
    EXPECT_THROW(ManhattanHeuristic<W>(-0.5), lpq::InvalidConfigError);
    EXPECT_THROW(HaversineHeuristic<W>(0.0), lpq::InvalidConfigError);
}

TEST(Heuristics, AdmissibleScaleIsDerivedFromTheGraph) {
    // On a unit grid with unit weights, weight/length is exactly 1 everywhere.
    const auto   g     = lpq_test::grid_graph<W>(10, 10);
    const double scale = lpq::euclidean_admissible_scale(g);
    EXPECT_NEAR(scale, 1.0, 1e-12);

    // Using the derived scale must preserve optimality.
    lpq::ReferenceDijkstraOptions ropts;
    ropts.target = 99;
    const auto expected = reference_dijkstra(g, 0, ropts);

    AstarOptions aopts;
    aopts.target       = 99;
    aopts.measure_time = false;
    EuclideanHeuristic<W> h(scale);
    Binary                q;
    q.reserve_nodes(g.num_nodes());
    const auto got = astar<W, Binary, EuclideanHeuristic<W>>(g, 0, q, h, aopts);
    EXPECT_EQ(got.dist[99], expected.dist[99]);
}

TEST(Heuristics, AdmissibleScaleIsZeroWithoutCoordinates) {
    EXPECT_EQ(lpq::euclidean_admissible_scale(lpq_test::diamond_graph()), 0.0);
}

// --- Haversine ----------------------------------------------------------------

/// A small geographic graph: four points around central London, connected in a
/// ring, with weights in metres derived from their true great-circle distances.
Graph london_ring() {
    // (longitude, latitude)
    std::vector<lpq::Coordinate> coords{
        {-0.1276, 51.5074},  // Westminster
        {-0.0877, 51.5155},  // Bank
        {-0.1419, 51.5014},  // Victoria
        {-0.1195, 51.5033},  // Waterloo
    };
    auto haversine_m = [](const lpq::Coordinate& a, const lpq::Coordinate& b) {
        constexpr double kDeg = 3.14159265358979323846 / 180.0;
        const double la1 = a.y * kDeg, la2 = b.y * kDeg;
        const double dla = la2 - la1;
        const double dlo = (b.x - a.x) * kDeg;
        const double s1 = std::sin(dla * 0.5), s2 = std::sin(dlo * 0.5);
        const double h = s1 * s1 + std::cos(la1) * std::cos(la2) * s2 * s2;
        return 2.0 * std::asin(std::sqrt(h)) * lpq::kEarthRadiusMetres;
    };

    std::vector<lpq::InputEdge<W>> edges;
    const int ring[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    for (const auto& e : ring) {
        // Ceil, so the arc weight is never below the great-circle distance and
        // the Haversine heuristic stays admissible.
        const auto w = static_cast<W>(
            std::ceil(haversine_m(coords[e[0]], coords[e[1]])));
        edges.push_back({static_cast<NodeId>(e[0]), static_cast<NodeId>(e[1]), w});
    }
    lpq::GraphBuildOptions bopts;
    bopts.directedness = lpq::Directedness::kUndirected;
    bopts.coord_system = CoordinateSystem::kGeographic;
    bopts.dataset_id   = "fixture:london-ring";
    return lpq::GraphBuilder<W>::build(4, std::move(edges), bopts, std::move(coords));
}

TEST(Heuristics, HaversinePreservesOptimalityOnGeographicData) {
    const auto g = london_ring();

    for (NodeId target = 0; target < 4; ++target) {
        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, 0, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;
        const auto got = astar<W, Binary, HaversineHeuristic<W>>(g, 0, aopts);

        EXPECT_EQ(got.dist[target], expected.dist[target]) << "target " << target;
    }
}

TEST(Heuristics, HaversineEstimateIsNeverAboveTheTrueArcCost) {
    // Direct admissibility check: for every arc, h(u) must not exceed
    // w(u,v) + h(v), which is the consistency condition.
    const auto g = london_ring();
    for (NodeId target = 0; target < 4; ++target) {
        HaversineHeuristic<W> h(1.0);
        h.prepare(g, target);
        for (NodeId u = 0; u < g.num_nodes(); ++u) {
            for (const auto& a : g.out_arcs(u)) {
                EXPECT_LE(h.estimate(u), a.weight + h.estimate(a.target))
                    << "consistency violated on arc " << u << "->" << a.target
                    << " for target " << target;
            }
        }
    }
}

TEST(Heuristics, HaversineIsZeroAtTheTarget) {
    // h(target) == 0 is part of the consistency definition and is what lets A*
    // stop when the target is extracted.
    const auto            g = london_ring();
    HaversineHeuristic<W> h(1.0);
    h.prepare(g, 2);
    EXPECT_EQ(h.estimate(2), 0U);
}

TEST(Heuristics, HaversineRejectsNonGeographicCoordinates) {
    const auto g = lpq_test::grid_graph<W>(4, 4);  // Euclidean, not geographic
    HaversineHeuristic<W> h(1.0);
    EXPECT_THROW(h.prepare(g, 0), lpq::UnsupportedConfigurationError);
}

TEST(Heuristics, HaversineHandlesIdenticalPoints) {
    // sin(0) terms make `a` exactly 0; sqrt and asin must not produce NaN.
    const auto            g = london_ring();
    HaversineHeuristic<W> h(1.0);
    h.prepare(g, 0);
    EXPECT_EQ(h.estimate(0), 0U);
}

// --- Randomised sweep ----------------------------------------------------------

TEST(Astar, RandomisedGridSweepPreservesOptimality) {
    for (std::uint64_t seed = 0; seed < 60; ++seed) {
        lpq::Rng   rng(seed + 77000);
        const auto dim = static_cast<std::size_t>(rng.next_in_range(3, 18));
        const auto g   = lpq_test::grid_graph<W>(dim, dim);

        const NodeId source = rng.next_node(g.num_nodes());
        const NodeId target = rng.next_node(g.num_nodes());

        lpq::ReferenceDijkstraOptions ropts;
        ropts.target = target;
        const auto expected = reference_dijkstra(g, source, ropts);

        AstarOptions aopts;
        aopts.target       = target;
        aopts.measure_time = false;

        const auto ctx = "seed=" + std::to_string(seed) + " " + std::to_string(source) +
                         "->" + std::to_string(target);

        ASSERT_EQ((astar<W, Binary, EuclideanHeuristic<W>>(g, source, aopts)).dist[target],
                  expected.dist[target]) << ctx << " [euclidean]";
        ASSERT_EQ((astar<W, Binary, ManhattanHeuristic<W>>(g, source, aopts)).dist[target],
                  expected.dist[target]) << ctx << " [manhattan]";
        ASSERT_EQ((astar<W, lpq::RadixHeap<Dist>, ManhattanHeuristic<W>>(g, source, aopts))
                      .dist[target],
                  expected.dist[target]) << ctx << " [radix+manhattan]";
    }
}

}  // namespace
