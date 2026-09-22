// =============================================================================
// Graph input and output (ESD.md section 26, PRD.md section 17).
//
// ESD.md section 26 fixes the pipeline:
//
//     download -> validate -> normalize -> convert to CSR -> compute metadata
//     -> generate query manifest -> store checksum
//
//     Never modify raw datasets in place.
//
// This header covers the conversion and checksum steps, and the readers for the
// two text formats the required datasets arrive in (PRD.md section 17: SNAP
// graph datasets and OpenStreetMap road networks, the latter distributed in the
// DIMACS shortest-path format).
//
// -----------------------------------------------------------------------------
// Why a binary format exists at all
// -----------------------------------------------------------------------------
//
// A continental road network is tens of millions of arcs. Parsing that from
// text costs tens of seconds, every run, and the parse is pure overhead: the
// text is not the object under study.
//
// Worse, it would land inside the experiment. PRD.md section 24 forbids work
// like this in timed regions, but the subtler problem is variance -- a parse
// that touches hundreds of megabytes evicts the entire cache before the search
// starts, so the first queries of a run would measure a cold machine and the
// later ones a warm one. Converting once, offline, removes both.
//
// The binary form is a direct image of the CSR arrays, so loading is three
// reads and no per-element work.
//
// -----------------------------------------------------------------------------
// Node identity is preserved, not discarded
// -----------------------------------------------------------------------------
//
// SNAP datasets carry arbitrary, sparse node ids -- a graph with 4,039 nodes
// may number them up to 10^6. CSR needs dense indices, so a remapping is
// unavoidable.
//
// The mapping is KEPT rather than thrown away. Without it a result referring to
// "node 1723" cannot be traced back to anything in the source dataset, and a
// query manifest generated against one conversion would silently mean something
// different after the file was reconverted. ESD.md section 26's checksum step
// exists for the same reason, one level up.
// =============================================================================
#ifndef LPQ_GRAPH_GRAPH_IO_HPP
#define LPQ_GRAPH_GRAPH_IO_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_builder.hpp"

namespace lpq {

/// Raised when a dataset file is malformed or cannot be read.
class GraphIoError : public LpqError {
public:
    explicit GraphIoError(const std::string& what) : LpqError(what) {}
};

/// "LPQGRAPH" as a little-endian 64-bit value.
inline constexpr std::uint64_t kGraphMagic = 0x485041524751504CULL;

inline constexpr std::uint32_t kGraphFormatVersion = 1;

/// Identifies the weight type a file was written with, so a file cannot be
/// loaded as the wrong one.
///
/// This check is not decoration. Reading 32-bit weights as 64-bit produces a
/// graph whose weights are arbitrary but plausible -- the search would run and
/// return confident, wrong distances.
enum class WeightTypeTag : std::uint32_t {
    kUInt32 = 1,
    kUInt64 = 2,
    kDouble = 3,
};

template <WeightType W>
[[nodiscard]] constexpr WeightTypeTag weight_type_tag() noexcept {
    if constexpr (std::is_same_v<W, std::uint32_t>) {
        return WeightTypeTag::kUInt32;
    } else if constexpr (std::is_same_v<W, std::uint64_t>) {
        return WeightTypeTag::kUInt64;
    } else {
        return WeightTypeTag::kDouble;
    }
}

/// The mapping from a dataset's own node identifiers to dense CSR indices.
struct NodeIdMapping {
    /// original_id[u] is the source dataset's identifier for CSR node u.
    std::vector<std::uint64_t> original_id;

    [[nodiscard]] bool empty() const noexcept { return original_id.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return original_id.size(); }
};

/// What a loaded dataset carries besides its arcs.
template <WeightType W>
struct LoadedGraph {
    CsrGraph<W>             graph;
    NodeIdMapping           mapping;
    std::vector<Coordinate> coordinates;

    /// FNV-1a over the arc payload. ESD.md section 26's "store checksum" step.
    std::uint64_t checksum = 0;
};

namespace detail {

/// FNV-1a. Chosen over a cryptographic hash because the threat here is
/// accidental corruption and silent format drift, not an adversary, and FNV-1a
/// costs one multiply per byte instead of a dependency.
inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

inline void hash_bytes(std::uint64_t& h, const void* data, std::size_t length) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        h ^= static_cast<std::uint64_t>(bytes[i]);
        h *= kFnvPrime;
    }
}

inline void put_u32_le(std::ostream& os, std::uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<char>((v >> (8 * i)) & 0xFFU);
    }
    os.write(b, 4);
}

inline void put_u64_le(std::ostream& os, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<char>((v >> (8 * i)) & 0xFFULL);
    }
    os.write(b, 8);
}

[[nodiscard]] inline std::uint32_t get_u32_le(std::istream& is) {
    char b[4];
    if (!is.read(b, 4)) {
        throw GraphIoError("graph file truncated while reading a 32-bit field");
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] inline std::uint64_t get_u64_le(std::istream& is) {
    char b[8];
    if (!is.read(b, 8)) {
        throw GraphIoError("graph file truncated while reading a 64-bit field");
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[i])) << (8 * i);
    }
    return v;
}

inline void put_f64_le(std::ostream& os, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64_le(os, bits);
}

[[nodiscard]] inline double get_f64_le(std::istream& is) {
    const std::uint64_t bits = get_u64_le(is);
    double              v    = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

template <WeightType W>
inline void put_weight(std::ostream& os, W w) {
    if constexpr (std::is_same_v<W, double>) {
        put_f64_le(os, w);
    } else if constexpr (std::is_same_v<W, std::uint64_t>) {
        put_u64_le(os, w);
    } else {
        put_u32_le(os, static_cast<std::uint32_t>(w));
    }
}

template <WeightType W>
[[nodiscard]] inline W get_weight(std::istream& is) {
    if constexpr (std::is_same_v<W, double>) {
        return get_f64_le(is);
    } else if constexpr (std::is_same_v<W, std::uint64_t>) {
        return get_u64_le(is);
    } else {
        return static_cast<W>(get_u32_le(is));
    }
}

}  // namespace detail

/// Checksums a graph's arc payload.
///
/// Computed over the CSR arrays in order, so it detects both a corrupted weight
/// and an arc that moved between adjacency lists -- the second being the fault
/// a per-edge checksum would miss.
template <WeightType W>
[[nodiscard]] std::uint64_t graph_checksum(const CsrGraph<W>& g) noexcept {
    std::uint64_t h = detail::kFnvOffset;

    const std::uint64_t n = g.num_nodes();
    detail::hash_bytes(h, &n, sizeof(n));

    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const Arc<W>& a : g.out_arcs(u)) {
            detail::hash_bytes(h, &u, sizeof(u));
            detail::hash_bytes(h, &a.target, sizeof(a.target));
            detail::hash_bytes(h, &a.weight, sizeof(a.weight));
        }
    }
    return h;
}

// -----------------------------------------------------------------------------
// Binary format
// -----------------------------------------------------------------------------

/// Writes a graph in the project's binary CSR format.
template <WeightType W>
void write_binary_graph(std::ostream& os, const CsrGraph<W>& g,
                        const NodeIdMapping&           mapping     = {},
                        const std::vector<Coordinate>& coordinates = {}) {
    if (!mapping.empty() && mapping.size() != g.num_nodes()) {
        throw GraphIoError("node id mapping has " + std::to_string(mapping.size()) +
                           " entries but the graph has " + std::to_string(g.num_nodes()) +
                           " nodes");
    }
    if (!coordinates.empty() && coordinates.size() != g.num_nodes()) {
        throw GraphIoError("coordinate array has " + std::to_string(coordinates.size()) +
                           " entries but the graph has " + std::to_string(g.num_nodes()) +
                           " nodes");
    }

    detail::put_u64_le(os, kGraphMagic);
    detail::put_u32_le(os, kGraphFormatVersion);
    detail::put_u32_le(os, static_cast<std::uint32_t>(weight_type_tag<W>()));
    detail::put_u64_le(os, static_cast<std::uint64_t>(g.num_nodes()));
    detail::put_u64_le(os, static_cast<std::uint64_t>(g.num_arcs()));
    detail::put_u32_le(os, mapping.empty() ? 0U : 1U);
    detail::put_u32_le(os, coordinates.empty() ? 0U : 1U);
    detail::put_u64_le(os, graph_checksum(g));

    // node_count + 1 offsets: each node's start, then the total arc count as
    // the final sentinel. arc_begin() is only valid for real nodes, so the
    // sentinel is written explicitly rather than by indexing one past the end.
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        detail::put_u64_le(os, static_cast<std::uint64_t>(g.arc_begin(u)));
    }
    detail::put_u64_le(os, static_cast<std::uint64_t>(g.num_arcs()));
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        for (const Arc<W>& a : g.out_arcs(u)) {
            detail::put_u32_le(os, a.target);
            detail::put_weight<W>(os, a.weight);
        }
    }
    if (!mapping.empty()) {
        for (std::uint64_t id : mapping.original_id) {
            detail::put_u64_le(os, id);
        }
    }
    if (!coordinates.empty()) {
        for (const Coordinate& c : coordinates) {
            detail::put_f64_le(os, c.x);
            detail::put_f64_le(os, c.y);
        }
    }
}

/// Reads a graph written by write_binary_graph.
///
/// Verifies the stored checksum against the loaded arcs and refuses a
/// mismatch. A silently corrupted graph would produce a search that runs
/// normally and returns wrong distances, which nothing downstream could detect.
template <WeightType W>
[[nodiscard]] LoadedGraph<W> read_binary_graph(std::istream& is) {
    if (detail::get_u64_le(is) != kGraphMagic) {
        throw GraphIoError("not an LPQ graph file (bad magic number)");
    }
    const std::uint32_t version = detail::get_u32_le(is);
    if (version != kGraphFormatVersion) {
        throw GraphIoError("graph format version " + std::to_string(version) +
                           " cannot be read by this build, which writes version " +
                           std::to_string(kGraphFormatVersion));
    }

    const std::uint32_t tag = detail::get_u32_le(is);
    if (tag != static_cast<std::uint32_t>(weight_type_tag<W>())) {
        throw GraphIoError(
            "graph file was written with a different weight type; loading it as this one "
            "would reinterpret every weight and the search would return confident, wrong "
            "distances");
    }

    const std::uint64_t node_count = detail::get_u64_le(is);
    const std::uint64_t arc_count  = detail::get_u64_le(is);
    if (node_count > kMaxNodeCount) {
        throw GraphIoError("graph file declares more nodes than NodeId can address");
    }

    const bool has_mapping     = detail::get_u32_le(is) != 0U;
    const bool has_coordinates = detail::get_u32_le(is) != 0U;
    const std::uint64_t stored_checksum = detail::get_u64_le(is);

    std::vector<std::uint64_t> offsets(static_cast<std::size_t>(node_count) + 1);
    for (std::uint64_t& o : offsets) {
        o = detail::get_u64_le(is);
    }
    if (!offsets.empty() && offsets.back() != arc_count) {
        throw GraphIoError("graph file's final offset disagrees with its declared arc count");
    }

    // Rebuilt through the ordinary builder rather than by assigning the arrays
    // directly, so a file cannot install a CSR structure that the builder's
    // validation would have rejected.
    std::vector<InputEdge<W>> edges;
    edges.reserve(static_cast<std::size_t>(arc_count));
    for (std::uint64_t u = 0; u + 1 < offsets.size(); ++u) {
        for (std::uint64_t i = offsets[u]; i < offsets[u + 1]; ++i) {
            const NodeId target = detail::get_u32_le(is);
            const W      weight = detail::get_weight<W>(is);
            edges.push_back(InputEdge<W>{static_cast<NodeId>(u), target, weight});
        }
    }

    LoadedGraph<W> loaded;
    if (has_mapping) {
        loaded.mapping.original_id.resize(static_cast<std::size_t>(node_count));
        for (std::uint64_t& id : loaded.mapping.original_id) {
            id = detail::get_u64_le(is);
        }
    }
    if (has_coordinates) {
        loaded.coordinates.resize(static_cast<std::size_t>(node_count));
        for (Coordinate& c : loaded.coordinates) {
            c.x = detail::get_f64_le(is);
            c.y = detail::get_f64_le(is);
        }
    }

    GraphBuildOptions opts;
    opts.directedness = Directedness::kDirected;  // arcs are already materialised
    opts.parallel_edges = ParallelEdgePolicy::kKeep;
    opts.self_loops     = SelfLoopPolicy::kKeep;
    loaded.graph        = GraphBuilder<W>::build(static_cast<std::size_t>(node_count),
                                                 std::move(edges), opts, loaded.coordinates);

    loaded.checksum = graph_checksum(loaded.graph);
    if (loaded.checksum != stored_checksum) {
        throw GraphIoError(
            "graph file checksum mismatch: the file is corrupt or was written by a "
            "different build. A silently corrupted graph would run normally and return "
            "wrong distances");
    }
    return loaded;
}

template <WeightType W>
void write_binary_graph_file(const std::string& path, const CsrGraph<W>& g,
                             const NodeIdMapping&           mapping     = {},
                             const std::vector<Coordinate>& coordinates = {}) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        throw GraphIoError("cannot open '" + path + "' for writing");
    }
    write_binary_graph(os, g, mapping, coordinates);
    os.flush();
    if (!os) {
        throw GraphIoError("failed while writing graph to '" + path + "'");
    }
}

template <WeightType W>
[[nodiscard]] LoadedGraph<W> read_binary_graph_file(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        throw GraphIoError("cannot open '" + path + "' for reading");
    }
    return read_binary_graph<W>(is);
}

// -----------------------------------------------------------------------------
// SNAP edge lists (PRD.md section 17)
// -----------------------------------------------------------------------------

struct SnapReadOptions {
    /// SNAP datasets are mostly unweighted. When a line carries no weight this
    /// value is used.
    std::uint64_t default_weight = 1;

    /// Most SNAP graphs are undirected and list each edge once.
    Directedness directedness = Directedness::kUndirected;

    SelfLoopPolicy     self_loops     = SelfLoopPolicy::kDrop;
    ParallelEdgePolicy parallel_edges = ParallelEdgePolicy::kKeep;
};

/// Reads a SNAP-style edge list: whitespace-separated "u v [w]", '#' comments.
///
/// Node ids are remapped to dense indices IN FIRST-APPEARANCE ORDER, and the
/// mapping is returned. First-appearance rather than sorted order because it is
/// stable under a file that is appended to, and because sorting would cost a
/// pass over a dataset that can be hundreds of megabytes.
template <WeightType W>
[[nodiscard]] LoadedGraph<W> read_snap_edge_list(std::istream& is,
                                                 const SnapReadOptions& options = {}) {
    std::unordered_map<std::uint64_t, NodeId> dense;
    std::vector<std::uint64_t>                original;
    std::vector<InputEdge<W>>                 edges;

    auto intern = [&dense, &original](std::uint64_t id) -> NodeId {
        const auto it = dense.find(id);
        if (it != dense.end()) {
            return it->second;
        }
        if (original.size() >= kMaxNodeCount) {
            throw GraphIoError("edge list contains more distinct nodes than NodeId can address");
        }
        const auto next = static_cast<NodeId>(original.size());
        dense.emplace(id, next);
        original.push_back(id);
        return next;
    };

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(is, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#' || line[0] == '%') {
            continue;
        }

        std::istringstream fields(line);
        std::uint64_t      u = 0;
        std::uint64_t      v = 0;
        if (!(fields >> u >> v)) {
            throw GraphIoError("malformed edge on line " + std::to_string(line_number) +
                               ": expected at least two node identifiers");
        }

        double weight_value = static_cast<double>(options.default_weight);
        if (!(fields >> weight_value)) {
            weight_value = static_cast<double>(options.default_weight);
        }
        if (weight_value < 0.0) {
            throw GraphIoError("negative weight on line " + std::to_string(line_number) +
                               "; Dijkstra requires non-negative weights");
        }

        edges.push_back(InputEdge<W>{intern(u), intern(v), static_cast<W>(weight_value)});
    }

    GraphBuildOptions opts;
    opts.directedness   = options.directedness;
    opts.self_loops     = options.self_loops;
    opts.parallel_edges = options.parallel_edges;

    LoadedGraph<W> loaded;
    loaded.mapping.original_id = std::move(original);
    loaded.graph = GraphBuilder<W>::build(loaded.mapping.size(), std::move(edges), opts);
    loaded.checksum = graph_checksum(loaded.graph);
    return loaded;
}

// -----------------------------------------------------------------------------
// DIMACS shortest-path format (the OSM road networks of PRD.md section 17)
// -----------------------------------------------------------------------------

/// Reads a DIMACS ".gr" arc file.
///
/// Format: "p sp <n> <m>" once, then "a <u> <v> <w>" per arc, with "c" comment
/// lines. Node ids are 1-based and dense, so unlike SNAP no remapping is
/// needed -- but the 1-based-to-0-based shift is, and getting it wrong shifts
/// every result by one node while leaving the graph structurally valid.
template <WeightType W>
[[nodiscard]] LoadedGraph<W> read_dimacs_graph(std::istream& is) {
    std::size_t               declared_nodes = 0;
    std::size_t               declared_arcs  = 0;
    bool                      seen_problem   = false;
    std::vector<InputEdge<W>> edges;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(is, line)) {
        ++line_number;
        if (line.empty() || line[0] == 'c') {
            continue;
        }

        std::istringstream fields(line);
        char               kind = 0;
        fields >> kind;

        if (kind == 'p') {
            std::string problem;
            if (!(fields >> problem >> declared_nodes >> declared_arcs)) {
                throw GraphIoError("malformed problem line " + std::to_string(line_number));
            }
            if (problem != "sp") {
                throw GraphIoError("DIMACS file declares problem type '" + problem +
                                   "'; only 'sp' (shortest path) is supported");
            }
            seen_problem = true;
            edges.reserve(declared_arcs);
            continue;
        }

        if (kind != 'a') {
            continue;  // unknown record types are ignored, as the format allows
        }
        if (!seen_problem) {
            throw GraphIoError("arc record on line " + std::to_string(line_number) +
                               " appears before the problem line");
        }

        std::uint64_t u = 0;
        std::uint64_t v = 0;
        double        w = 0.0;
        if (!(fields >> u >> v >> w)) {
            throw GraphIoError("malformed arc on line " + std::to_string(line_number));
        }
        if (u == 0 || v == 0) {
            throw GraphIoError("DIMACS node ids are 1-based; line " +
                               std::to_string(line_number) + " uses 0");
        }
        if (u > declared_nodes || v > declared_nodes) {
            throw GraphIoError("arc on line " + std::to_string(line_number) +
                               " refers to a node beyond the declared count");
        }
        if (w < 0.0) {
            throw GraphIoError("negative arc weight on line " + std::to_string(line_number));
        }

        edges.push_back(InputEdge<W>{static_cast<NodeId>(u - 1), static_cast<NodeId>(v - 1),
                                     static_cast<W>(w)});
    }

    if (!seen_problem) {
        throw GraphIoError("DIMACS file contains no problem line");
    }

    GraphBuildOptions opts;
    // DIMACS arc files list each direction explicitly, so the graph is directed
    // even when the underlying road network is not. Treating it as undirected
    // would double every arc.
    opts.directedness   = Directedness::kDirected;
    opts.self_loops     = SelfLoopPolicy::kKeep;
    opts.parallel_edges = ParallelEdgePolicy::kKeep;

    LoadedGraph<W> loaded;
    loaded.graph    = GraphBuilder<W>::build(declared_nodes, std::move(edges), opts);
    loaded.checksum = graph_checksum(loaded.graph);
    return loaded;
}

/// Reads a DIMACS ".co" coordinate file into `coordinates`.
///
/// Format: "v <id> <x> <y>", with coordinates in microdegrees. They are kept in
/// the file's own units rather than converted here: the A* heuristics take a
/// scale factor, and converting at load would bake one interpretation into the
/// data where a later experiment might need another.
inline void read_dimacs_coordinates(std::istream& is, std::size_t node_count,
                                    std::vector<Coordinate>& coordinates) {
    coordinates.assign(node_count, Coordinate{});
    std::vector<bool> seen(node_count, false);

    std::string line;
    std::size_t line_number = 0;
    std::size_t filled      = 0;
    while (std::getline(is, line)) {
        ++line_number;
        if (line.empty() || line[0] == 'c' || line[0] == 'p') {
            continue;
        }

        std::istringstream fields(line);
        char               kind = 0;
        fields >> kind;
        if (kind != 'v') {
            continue;
        }

        std::uint64_t id = 0;
        double        x  = 0.0;
        double        y  = 0.0;
        if (!(fields >> id >> x >> y)) {
            throw GraphIoError("malformed coordinate on line " + std::to_string(line_number));
        }
        if (id == 0 || id > node_count) {
            throw GraphIoError("coordinate on line " + std::to_string(line_number) +
                               " refers to a node outside the graph");
        }

        const std::size_t at = static_cast<std::size_t>(id - 1);
        coordinates[at]      = Coordinate{x, y};
        if (!seen[at]) {
            seen[at] = true;
            ++filled;
        }
    }

    if (filled != node_count) {
        // A partially populated coordinate array is worse than none: A* would
        // run with a heuristic of zero for the missing nodes and a real one
        // elsewhere, which is still admissible and so would produce correct but
        // inexplicably slow results.
        throw GraphIoError("coordinate file covers " + std::to_string(filled) + " of " +
                           std::to_string(node_count) +
                           " nodes; a partial coordinate set would leave A* with a "
                           "heuristic of zero for the remainder");
    }
}

}  // namespace lpq

#endif  // LPQ_GRAPH_GRAPH_IO_HPP
