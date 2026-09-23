// =============================================================================
// Graph input and output (ESD.md section 26, PRD.md section 17).
//
// Most of these tests exist to prove a loader REJECTS something. A dataset that
// loads and is silently wrong is the dangerous case: the search runs normally
// and returns confident, wrong distances, and nothing downstream can tell.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;

std::stringstream binary_stream() {
    return std::stringstream(std::ios::in | std::ios::out | std::ios::binary);
}

// -----------------------------------------------------------------------------
// Checksums
// -----------------------------------------------------------------------------

TEST(GraphChecksum, IsStableForTheSameGraph) {
    const CsrGraph<Weight> g = lpq_test::grid_graph<Weight>(5, 5);
    EXPECT_EQ(graph_checksum(g), graph_checksum(g));
}

TEST(GraphChecksum, ChangesWhenAWeightChanges) {
    const CsrGraph<Weight> a = lpq_test::path_graph<Weight>(10, 1);
    const CsrGraph<Weight> b = lpq_test::path_graph<Weight>(10, 2);
    EXPECT_NE(graph_checksum(a), graph_checksum(b));
}

// The fault a per-edge checksum would miss: the same arcs, attached to
// different sources.
TEST(GraphChecksum, ChangesWhenAnArcMovesBetweenAdjacencyLists) {
    const std::vector<InputEdge<Weight>> left  = {{0, 1, 5}, {2, 3, 7}};
    const std::vector<InputEdge<Weight>> right = {{0, 3, 5}, {2, 1, 7}};

    GraphBuildOptions opts;
    const CsrGraph<Weight> a = GraphBuilder<Weight>::build(4, left, opts);
    const CsrGraph<Weight> b = GraphBuilder<Weight>::build(4, right, opts);

    EXPECT_NE(graph_checksum(a), graph_checksum(b));
}

// -----------------------------------------------------------------------------
// Binary round trips
// -----------------------------------------------------------------------------

void expect_same_graph(const CsrGraph<Weight>& a, const CsrGraph<Weight>& b) {
    ASSERT_EQ(a.num_nodes(), b.num_nodes());
    ASSERT_EQ(a.num_arcs(), b.num_arcs());
    for (NodeId u = 0; u < a.num_nodes(); ++u) {
        const auto left  = a.out_arcs(u);
        const auto right = b.out_arcs(u);
        ASSERT_EQ(left.size(), right.size()) << "node " << u;
        for (std::size_t i = 0; i < left.size(); ++i) {
            EXPECT_EQ(left[i].target, right[i].target) << "node " << u << " arc " << i;
            EXPECT_EQ(left[i].weight, right[i].weight) << "node " << u << " arc " << i;
        }
    }
}

TEST(BinaryGraph, RoundTripsAFixtureGraph) {
    const CsrGraph<Weight> original = lpq_test::grid_graph<Weight>(8, 6);

    auto s = binary_stream();
    write_binary_graph<Weight>(s, original);
    s.seekg(0);

    const LoadedGraph<Weight> loaded = read_binary_graph<Weight>(s);
    expect_same_graph(original, loaded.graph);
    EXPECT_EQ(loaded.checksum, graph_checksum(original));
}

TEST(BinaryGraph, RoundTripsAnEmptyGraph) {
    GraphBuildOptions      opts;
    const CsrGraph<Weight> original =
        GraphBuilder<Weight>::build(0, std::vector<InputEdge<Weight>>{}, opts);

    auto s = binary_stream();
    write_binary_graph<Weight>(s, original);
    s.seekg(0);

    const LoadedGraph<Weight> loaded = read_binary_graph<Weight>(s);
    EXPECT_EQ(loaded.graph.num_nodes(), 0U);
    EXPECT_EQ(loaded.graph.num_arcs(), 0U);
}

TEST(BinaryGraph, RoundTripsIsolatedNodes) {
    // A node with no arcs is a legal input and a common one in real datasets;
    // it exercises the offset array's repeated values.
    GraphBuildOptions            opts;
    std::vector<InputEdge<Weight>> edges = {{0, 1, 4}};
    const CsrGraph<Weight> original = GraphBuilder<Weight>::build(6, edges, opts);

    auto s = binary_stream();
    write_binary_graph<Weight>(s, original);
    s.seekg(0);

    expect_same_graph(original, read_binary_graph<Weight>(s).graph);
}

TEST(BinaryGraph, RoundTripsTheNodeIdMapping) {
    const CsrGraph<Weight> original = lpq_test::path_graph<Weight>(4);
    NodeIdMapping          mapping;
    mapping.original_id = {900, 12, 77, 40000000000ULL};

    auto s = binary_stream();
    write_binary_graph<Weight>(s, original, mapping);
    s.seekg(0);

    const LoadedGraph<Weight> loaded = read_binary_graph<Weight>(s);
    ASSERT_EQ(loaded.mapping.size(), 4U);
    EXPECT_EQ(loaded.mapping.original_id[0], 900U);
    EXPECT_EQ(loaded.mapping.original_id[3], 40000000000ULL);
}

TEST(BinaryGraph, RoundTripsCoordinates) {
    const CsrGraph<Weight> original = lpq_test::path_graph<Weight>(3);
    const std::vector<Coordinate> coords = {{1.5, -2.5}, {0.0, 0.0}, {1e9, -1e9}};

    auto s = binary_stream();
    write_binary_graph<Weight>(s, original, {}, coords);
    s.seekg(0);

    const LoadedGraph<Weight> loaded = read_binary_graph<Weight>(s);
    ASSERT_EQ(loaded.coordinates.size(), 3U);
    EXPECT_EQ(loaded.coordinates[0].x, 1.5);
    EXPECT_EQ(loaded.coordinates[2].y, -1e9);
}

TEST(BinaryGraph, RejectsAMappingOfTheWrongLength) {
    const CsrGraph<Weight> g = lpq_test::path_graph<Weight>(4);
    NodeIdMapping          mapping;
    mapping.original_id = {1, 2};

    auto s = binary_stream();
    EXPECT_THROW(write_binary_graph<Weight>(s, g, mapping), GraphIoError);
}

// -----------------------------------------------------------------------------
// Binary rejections
// -----------------------------------------------------------------------------

TEST(BinaryGraph, RejectsAFileThatIsNotAGraph) {
    auto s = binary_stream();
    s << "this is a text file";
    s.seekg(0);
    EXPECT_THROW((void)read_binary_graph<Weight>(s), GraphIoError);
}

TEST(BinaryGraph, RejectsAnEmptyFile) {
    auto s = binary_stream();
    EXPECT_THROW((void)read_binary_graph<Weight>(s), GraphIoError);
}

// Reading 32-bit weights as 64-bit yields a graph whose weights are arbitrary
// but plausible, so the search would run and return confident, wrong distances.
TEST(BinaryGraph, RefusesToLoadAGraphAsTheWrongWeightType) {
    const CsrGraph<std::uint32_t> g = lpq_test::grid_graph<std::uint32_t>(4, 4);

    auto s = binary_stream();
    write_binary_graph<std::uint32_t>(s, g);
    s.seekg(0);

    EXPECT_THROW((void)read_binary_graph<std::uint64_t>(s), GraphIoError);
}

TEST(BinaryGraph, RejectsATruncatedFile) {
    const CsrGraph<Weight> g = lpq_test::grid_graph<Weight>(6, 6);

    auto s = binary_stream();
    write_binary_graph<Weight>(s, g);

    std::string bytes = s.str();
    bytes.resize(bytes.size() / 2);

    auto truncated = binary_stream();
    truncated.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    truncated.seekg(0);

    EXPECT_THROW((void)read_binary_graph<Weight>(truncated), GraphIoError);
}

/// Writes a graph, flips one bit at `offset_from_end`, and returns the stream.
std::stringstream corrupt_one_byte(const CsrGraph<Weight>& g, std::size_t offset_from_end,
                                   unsigned char mask) {
    auto s = binary_stream();
    write_binary_graph<Weight>(s, g);

    std::string bytes = s.str();
    const std::size_t at = bytes.size() - offset_from_end;
    bytes[at] = static_cast<char>(static_cast<unsigned char>(bytes[at]) ^ mask);

    auto corrupted = binary_stream();
    corrupted.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    corrupted.seekg(0);
    return corrupted;
}

// Corruption is caught by two independent layers, and which layer fires depends
// on what was hit. Both are tested, because a graph that loads and is silently
// wrong is the only genuinely dangerous outcome here.

// A damaged WEIGHT stays structurally valid -- any 32-bit value is a legal
// weight -- so only the checksum can catch it.
TEST(BinaryGraph, TheChecksumDetectsACorruptedWeight) {
    const CsrGraph<Weight> g = lpq_test::grid_graph<Weight>(5, 5);

    // The last arc's weight occupies the final four bytes; byte 1 from the end
    // is its most significant.
    std::stringstream corrupted = corrupt_one_byte(g, 1, 0x40);

    EXPECT_THROW((void)read_binary_graph<Weight>(corrupted), GraphIoError);
}

// A damaged TARGET is caught earlier and more strongly, by the structural
// validation the graph is rebuilt through. That is why loading reconstructs the
// graph via the ordinary builder instead of assigning the CSR arrays directly:
// a file cannot install a structure the builder would have rejected.
TEST(BinaryGraph, StructuralValidationDetectsACorruptedArcTarget) {
    const CsrGraph<Weight> g = lpq_test::grid_graph<Weight>(5, 5);

    // Byte 5 from the end is the most significant byte of the last arc's
    // target, which the flip drives far beyond the node count.
    std::stringstream corrupted = corrupt_one_byte(g, 5, 0x40);

    EXPECT_THROW((void)read_binary_graph<Weight>(corrupted), InvalidGraphError);
}

TEST(BinaryGraph, ReportsAMissingFile) {
    EXPECT_THROW((void)read_binary_graph_file<Weight>("no/such/directory/graph.lpqg"),
                 GraphIoError);
}

// -----------------------------------------------------------------------------
// SNAP edge lists
// -----------------------------------------------------------------------------

TEST(SnapEdgeList, ReadsAnUnweightedUndirectedList) {
    std::stringstream s;
    s << "# Directed graph: something.txt\n";
    s << "# Nodes: 3 Edges: 2\n";
    s << "0\t1\n";
    s << "1\t2\n";

    const LoadedGraph<Weight> loaded = read_snap_edge_list<Weight>(s);
    EXPECT_EQ(loaded.graph.num_nodes(), 3U);
    EXPECT_EQ(loaded.graph.num_arcs(), 4U);  // undirected: each edge both ways
    EXPECT_EQ(loaded.graph.out_arcs(1).size(), 2U);
}

// SNAP ids are arbitrary and sparse; a graph with three nodes may number them
// in the millions.
TEST(SnapEdgeList, RemapsSparseIdentifiersAndKeepsTheMapping) {
    std::stringstream s;
    s << "1000000 5\n";
    s << "5 42\n";

    const LoadedGraph<Weight> loaded = read_snap_edge_list<Weight>(s);
    EXPECT_EQ(loaded.graph.num_nodes(), 3U);

    // First-appearance order, which is stable under a file that is appended to.
    ASSERT_EQ(loaded.mapping.size(), 3U);
    EXPECT_EQ(loaded.mapping.original_id[0], 1000000U);
    EXPECT_EQ(loaded.mapping.original_id[1], 5U);
    EXPECT_EQ(loaded.mapping.original_id[2], 42U);
}

TEST(SnapEdgeList, ReadsAnOptionalThirdColumnAsAWeight) {
    std::stringstream s;
    s << "0 1 17\n";

    const LoadedGraph<Weight> loaded = read_snap_edge_list<Weight>(s);
    ASSERT_EQ(loaded.graph.out_arcs(0).size(), 1U);
    EXPECT_EQ(loaded.graph.out_arcs(0)[0].weight, 17U);
}

TEST(SnapEdgeList, UsesTheDefaultWeightWhenNoneIsGiven) {
    std::stringstream s;
    s << "0 1\n";

    SnapReadOptions options;
    options.default_weight = 9;

    const LoadedGraph<Weight> loaded = read_snap_edge_list<Weight>(s, options);
    EXPECT_EQ(loaded.graph.out_arcs(0)[0].weight, 9U);
}

TEST(SnapEdgeList, SkipsCommentsAndBlankLines) {
    std::stringstream s;
    s << "# a comment\n";
    s << "% another comment style\n";
    s << "\n";
    s << "0 1\n";

    EXPECT_EQ(read_snap_edge_list<Weight>(s).graph.num_nodes(), 2U);
}

TEST(SnapEdgeList, RejectsAMalformedLine) {
    std::stringstream s;
    s << "0\n";  // only one identifier
    EXPECT_THROW((void)read_snap_edge_list<Weight>(s), GraphIoError);
}

TEST(SnapEdgeList, RejectsANegativeWeight) {
    // Dijkstra requires non-negative weights; accepting one would produce
    // wrong answers with no error.
    std::stringstream s;
    s << "0 1 -5\n";
    EXPECT_THROW((void)read_snap_edge_list<Weight>(s), GraphIoError);
}

TEST(SnapEdgeList, DropsSelfLoopsByDefault) {
    std::stringstream s;
    s << "0 0\n";
    s << "0 1\n";

    const LoadedGraph<Weight> loaded = read_snap_edge_list<Weight>(s);
    for (const Arc<Weight>& a : loaded.graph.out_arcs(0)) {
        EXPECT_NE(a.target, 0U);
    }
}

// -----------------------------------------------------------------------------
// DIMACS (the OSM road networks of PRD.md section 17)
// -----------------------------------------------------------------------------

TEST(DimacsGraph, ReadsArcsAndConvertsFromOneBasedIdentifiers) {
    // Getting the 1-based shift wrong moves every result by one node while
    // leaving the graph structurally valid.
    std::stringstream s;
    s << "c a comment\n";
    s << "p sp 3 2\n";
    s << "a 1 2 10\n";
    s << "a 2 3 20\n";

    const LoadedGraph<Weight> loaded = read_dimacs_graph<Weight>(s);
    EXPECT_EQ(loaded.graph.num_nodes(), 3U);
    EXPECT_EQ(loaded.graph.num_arcs(), 2U);

    ASSERT_EQ(loaded.graph.out_arcs(0).size(), 1U);
    EXPECT_EQ(loaded.graph.out_arcs(0)[0].target, 1U);
    EXPECT_EQ(loaded.graph.out_arcs(0)[0].weight, 10U);
}

// DIMACS arc files list each direction explicitly, so treating the file as
// undirected would double every arc.
TEST(DimacsGraph, TreatsTheFileAsDirected) {
    std::stringstream s;
    s << "p sp 2 2\n";
    s << "a 1 2 5\n";
    s << "a 2 1 5\n";

    const LoadedGraph<Weight> loaded = read_dimacs_graph<Weight>(s);
    EXPECT_EQ(loaded.graph.num_arcs(), 2U);
}

TEST(DimacsGraph, RejectsAFileWithNoProblemLine) {
    std::stringstream s;
    s << "a 1 2 5\n";
    EXPECT_THROW((void)read_dimacs_graph<Weight>(s), GraphIoError);
}

TEST(DimacsGraph, RejectsANodeIdentifierOfZero) {
    std::stringstream s;
    s << "p sp 2 1\n";
    s << "a 0 1 5\n";
    EXPECT_THROW((void)read_dimacs_graph<Weight>(s), GraphIoError);
}

TEST(DimacsGraph, RejectsAnArcBeyondTheDeclaredNodeCount) {
    std::stringstream s;
    s << "p sp 2 1\n";
    s << "a 1 99 5\n";
    EXPECT_THROW((void)read_dimacs_graph<Weight>(s), GraphIoError);
}

TEST(DimacsGraph, RejectsANegativeArcWeight) {
    std::stringstream s;
    s << "p sp 2 1\n";
    s << "a 1 2 -5\n";
    EXPECT_THROW((void)read_dimacs_graph<Weight>(s), GraphIoError);
}

TEST(DimacsGraph, RejectsANonShortestPathProblem) {
    std::stringstream s;
    s << "p max 2 1\n";
    EXPECT_THROW((void)read_dimacs_graph<Weight>(s), GraphIoError);
}

TEST(DimacsCoordinates, ReadsAllNodes) {
    std::stringstream s;
    s << "c comment\n";
    s << "v 1 100 200\n";
    s << "v 2 300 400\n";

    std::vector<Coordinate> coords;
    read_dimacs_coordinates(s, 2, coords);

    ASSERT_EQ(coords.size(), 2U);
    EXPECT_EQ(coords[0].x, 100.0);
    EXPECT_EQ(coords[1].y, 400.0);
}

// A partial coordinate set is worse than none: A* would run with a heuristic of
// zero for the missing nodes and a real one elsewhere, which stays admissible
// and so produces correct but inexplicably slow results.
TEST(DimacsCoordinates, RejectsAPartialCoordinateSet) {
    std::stringstream s;
    s << "v 1 100 200\n";

    std::vector<Coordinate> coords;
    EXPECT_THROW(read_dimacs_coordinates(s, 5, coords), GraphIoError);
}

TEST(DimacsCoordinates, RejectsACoordinateOutsideTheGraph) {
    std::stringstream s;
    s << "v 99 100 200\n";

    std::vector<Coordinate> coords;
    EXPECT_THROW(read_dimacs_coordinates(s, 2, coords), GraphIoError);
}

// -----------------------------------------------------------------------------
// Conversion preserves the graph
// -----------------------------------------------------------------------------

// The pipeline step ESD.md section 26 calls "convert to CSR": whatever a text
// reader produced must survive the binary round trip exactly, or the converted
// dataset is not the dataset that was downloaded.
TEST(GraphIo, TextToBinaryConversionIsLossless) {
    Rng rng(20260922U);

    for (int trial = 0; trial < 200; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 2;
        spec.max_nodes        = 40;
        spec.edge_probability = 0.2;
        spec.max_weight       = 1000;
        const CsrGraph<Weight> source = lpq_test::random_graph(rng, spec);

        // Emit it as a DIMACS file, read it back, then binary round trip.
        std::stringstream text;
        text << "p sp " << source.num_nodes() << " " << source.num_arcs() << "\n";
        for (NodeId u = 0; u < source.num_nodes(); ++u) {
            for (const Arc<Weight>& a : source.out_arcs(u)) {
                text << "a " << (u + 1) << " " << (a.target + 1) << " " << a.weight << "\n";
            }
        }

        const LoadedGraph<Weight> from_text = read_dimacs_graph<Weight>(text);
        ASSERT_EQ(from_text.graph.num_arcs(), source.num_arcs()) << "trial " << trial;

        auto binary = binary_stream();
        write_binary_graph<Weight>(binary, from_text.graph);
        binary.seekg(0);
        const LoadedGraph<Weight> from_binary = read_binary_graph<Weight>(binary);

        ASSERT_EQ(from_binary.checksum, from_text.checksum) << "trial " << trial;
        expect_same_graph(from_text.graph, from_binary.graph);
    }
}


// -----------------------------------------------------------------------------
// Directedness survives the round trip (format version 2)
// -----------------------------------------------------------------------------
//
// Regression: version 1 of the format carried no directedness field, so a graph
// written as undirected came back labelled directed. Nothing crashed -- the
// arcs were all still there -- but num_edges() halves the arc count only for
// undirected graphs, so every metadata record computed after a conversion
// reported twice the edges the dataset actually had.

TEST(GraphIo, AnUndirectedGraphIsStillUndirectedAfterARoundTrip) {
    std::vector<InputEdge<std::uint32_t>> edges = {
        {0, 1, 5}, {1, 2, 7}, {2, 3, 9},
    };
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    const CsrGraph<std::uint32_t> g =
        GraphBuilder<std::uint32_t>::build(4, std::move(edges), opts);

    ASSERT_EQ(g.num_arcs(), 6U);
    ASSERT_EQ(g.num_edges(), 3U);

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_binary_graph<std::uint32_t>(buffer, g, {}, {});
    const LoadedGraph<std::uint32_t> back = read_binary_graph<std::uint32_t>(buffer);

    EXPECT_EQ(back.graph.directedness(), Directedness::kUndirected);
    EXPECT_EQ(back.graph.num_arcs(), g.num_arcs());
    EXPECT_EQ(back.graph.num_edges(), g.num_edges());
    // And the arcs must not have been paired a second time on the way back in.
    EXPECT_EQ(back.graph.num_arcs(), 6U);
}

TEST(GraphIo, ADirectedGraphIsStillDirectedAfterARoundTrip) {
    std::vector<InputEdge<std::uint32_t>> edges = {{0, 1, 5}, {1, 2, 7}};
    GraphBuildOptions opts;
    opts.directedness = Directedness::kDirected;
    const CsrGraph<std::uint32_t> g =
        GraphBuilder<std::uint32_t>::build(3, std::move(edges), opts);

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    write_binary_graph<std::uint32_t>(buffer, g, {}, {});
    const LoadedGraph<std::uint32_t> back = read_binary_graph<std::uint32_t>(buffer);

    EXPECT_EQ(back.graph.directedness(), Directedness::kDirected);
    EXPECT_EQ(back.graph.num_arcs(), 2U);
    EXPECT_EQ(back.graph.num_edges(), 2U);
}


// -----------------------------------------------------------------------------
// Edge listing convention (regression)
// -----------------------------------------------------------------------------
//
// SNAP does not state, and is not consistent about, whether an undirected edge
// appears once or in both directions. facebook_combined.txt lists each edge
// once (88,234 records for the 88,234 edges SNAP publishes); roadNet-PA.txt
// lists both (3,083,796 records for 1,541,898 edges). Reading the second as
// though it were the first produces a graph with exactly twice the edges --
// structurally valid, silent, and wrong in every per-edge measurement taken on
// it. It is declared, not inferred, because a directed graph that happens to be
// symmetric is indistinguishable from a doubly-listed undirected one.

TEST(SnapReader, PairsEdgesWhenTheFileListsEachEdgeOnce) {
    std::istringstream input("1 2\n2 3\n");
    SnapReadOptions    options;
    options.directedness                 = Directedness::kUndirected;
    options.edges_listed_both_directions = false;

    const LoadedGraph<std::uint32_t> loaded = read_snap_edge_list<std::uint32_t>(input, options);
    EXPECT_EQ(loaded.graph.num_nodes(), 3U);
    EXPECT_EQ(loaded.graph.num_arcs(), 4U);
    EXPECT_EQ(loaded.graph.num_edges(), 2U);
}

TEST(SnapReader, DoesNotPairAgainWhenTheFileAlreadyListsBothDirections) {
    std::istringstream input("1 2\n2 1\n2 3\n3 2\n");
    SnapReadOptions    options;
    options.directedness                 = Directedness::kUndirected;
    options.edges_listed_both_directions = true;

    const LoadedGraph<std::uint32_t> loaded = read_snap_edge_list<std::uint32_t>(input, options);
    EXPECT_EQ(loaded.graph.num_nodes(), 3U);
    // Four arcs, not eight.
    EXPECT_EQ(loaded.graph.num_arcs(), 4U);
    EXPECT_EQ(loaded.graph.num_edges(), 2U);
    // The label is kept, which is what makes num_edges() halve the arc count.
    EXPECT_EQ(loaded.graph.directedness(), Directedness::kUndirected);
}

TEST(SnapReader, TheTwoConventionsAgreeOnTheResultingGraph) {
    // The same graph described both ways must convert to the same thing --
    // including the same checksum, since that is what downstream records key on.
    std::istringstream once("1 2\n2 3\n3 1\n");
    std::istringstream both("1 2\n2 1\n2 3\n3 2\n3 1\n1 3\n");

    SnapReadOptions once_options;
    once_options.directedness                 = Directedness::kUndirected;
    once_options.edges_listed_both_directions = false;

    SnapReadOptions both_options;
    both_options.directedness                 = Directedness::kUndirected;
    both_options.edges_listed_both_directions = true;

    const LoadedGraph<std::uint32_t> a = read_snap_edge_list<std::uint32_t>(once, once_options);
    const LoadedGraph<std::uint32_t> b = read_snap_edge_list<std::uint32_t>(both, both_options);

    EXPECT_EQ(a.graph.num_nodes(), b.graph.num_nodes());
    EXPECT_EQ(a.graph.num_arcs(), b.graph.num_arcs());
    EXPECT_EQ(a.graph.num_edges(), b.graph.num_edges());
    EXPECT_EQ(a.checksum, b.checksum);
}

TEST(SnapReader, TheFlagIsIgnoredForADirectedGraph) {
    // Nothing is paired in the directed case, so the flag has nothing to
    // suppress and must not change the result.
    std::istringstream input("1 2\n2 3\n");
    SnapReadOptions    options;
    options.directedness                 = Directedness::kDirected;
    options.edges_listed_both_directions = true;

    const LoadedGraph<std::uint32_t> loaded = read_snap_edge_list<std::uint32_t>(input, options);
    EXPECT_EQ(loaded.graph.num_arcs(), 2U);
    EXPECT_EQ(loaded.graph.directedness(), Directedness::kDirected);
}

}  // namespace
}  // namespace lpq
