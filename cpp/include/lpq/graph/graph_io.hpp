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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
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

/// Version 2 added the directedness field. Version 1 files cannot be read:
/// they carry no directedness, and guessing one would make num_edges() report
/// either double or half the truth with no way to tell which.
///
/// Version 3 replaced the coordinate *flag* with the coordinate *system*.
/// Version 2 stored only "are there coordinates", so a loaded graph got its
/// coordinate array back but not its system, and CsrGraph::has_coordinates()
/// requires both. Every graph written to disk therefore reported having no
/// coordinates, which silently made every heuristic except `zero` unusable
/// from a saved file -- A* was reachable only from an in-memory graph. The
/// system has to be in the file because it cannot be recovered from the
/// numbers: the same pair of doubles is a valid plane point and a valid
/// lat/lon.
inline constexpr std::uint32_t kGraphFormatVersion = 3;

/// The oldest version this build can still read. Version 2 files are accepted
/// because their coordinate flag is recoverable: every version-2 file this
/// project produced took its coordinates from a DIMACS `.co` file, which
/// lpq_convert labels geographic, so a set flag means kGeographic.
inline constexpr std::uint32_t kMinReadableGraphFormatVersion = 2;

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

/// Bytes one arc occupies on disk: a 32-bit target and the weight as
/// put_weight writes it.
template <WeightType W>
[[nodiscard]] constexpr std::uint64_t arc_record_bytes() noexcept {
    return 4U + ((std::is_same_v<W, double> || std::is_same_v<W, std::uint64_t>) ? 8U : 4U);
}

/// Bytes between the read position and the end of `is`, or nullopt when the
/// stream cannot say (a pipe). The read position is left where it was.
[[nodiscard]] inline std::optional<std::uint64_t> remaining_bytes(std::istream& is) {
    const std::streampos here = is.tellg();
    if (here == std::streampos(-1)) {
        is.clear();
        return std::nullopt;
    }
    is.seekg(0, std::ios::end);
    const std::streampos end = is.tellg();
    is.clear();
    is.seekg(here);
    if (end == std::streampos(-1) || end < here) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(end - here);
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
///
/// `coord_system` describes the supplied coordinates. When left at kNone it is
/// taken from the graph. Writing coordinates whose system is kNone is refused
/// rather than defaulted: that combination is exactly what version 2 stored,
/// and it reads back as a graph that claims to have no coordinates.
template <WeightType W>
void write_binary_graph(std::ostream& os, const CsrGraph<W>& g,
                        const NodeIdMapping&           mapping      = {},
                        const std::vector<Coordinate>& coordinates  = {},
                        CoordinateSystem               coord_system = CoordinateSystem::kNone) {
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

    // An explicit argument wins; otherwise the graph is asked. A caller that
    // supplies coordinates for a graph that has no system has not said what
    // the numbers mean, and nothing downstream could recover it.
    const CoordinateSystem system =
        coord_system != CoordinateSystem::kNone ? coord_system : g.coordinate_system();
    if (!coordinates.empty() && system == CoordinateSystem::kNone) {
        throw GraphIoError(
            "coordinates were supplied with no coordinate system; the file would read "
            "back as a graph with no usable coordinates and every heuristic except "
            "'zero' would silently become unavailable");
    }

    detail::put_u64_le(os, kGraphMagic);
    detail::put_u32_le(os, kGraphFormatVersion);
    detail::put_u32_le(os, static_cast<std::uint32_t>(weight_type_tag<W>()));
    detail::put_u32_le(os, g.directedness() == Directedness::kUndirected ? 1U : 0U);
    detail::put_u64_le(os, static_cast<std::uint64_t>(g.num_nodes()));
    detail::put_u64_le(os, static_cast<std::uint64_t>(g.num_arcs()));
    detail::put_u32_le(os, mapping.empty() ? 0U : 1U);
    detail::put_u32_le(os, coordinates.empty() ? 0U : static_cast<std::uint32_t>(system));
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
    if (version < kMinReadableGraphFormatVersion || version > kGraphFormatVersion) {
        throw GraphIoError("graph format version " + std::to_string(version) +
                           " cannot be read by this build, which writes version " +
                           std::to_string(kGraphFormatVersion) + " and reads back to " +
                           std::to_string(kMinReadableGraphFormatVersion));
    }

    const std::uint32_t tag = detail::get_u32_le(is);
    if (tag != static_cast<std::uint32_t>(weight_type_tag<W>())) {
        throw GraphIoError(
            "graph file was written with a different weight type; loading it as this one "
            "would reinterpret every weight and the search would return confident, wrong "
            "distances");
    }

    const bool          undirected = detail::get_u32_le(is) != 0U;
    const std::uint64_t node_count  = detail::get_u64_le(is);
    const std::uint64_t arc_count   = detail::get_u64_le(is);
    if (node_count > kMaxNodeCount) {
        throw GraphIoError("graph file declares more nodes than NodeId can address");
    }

    const bool          has_mapping = detail::get_u32_le(is) != 0U;
    const std::uint32_t coord_field = detail::get_u32_le(is);

    // Version 2 wrote a bare 0/1 flag. Every version-2 file this project
    // produced took its coordinates from a DIMACS .co file, which lpq_convert
    // labels geographic, so a set flag is kGeographic. Version 3 onward stores
    // the system itself and nothing has to be inferred.
    CoordinateSystem coord_system = CoordinateSystem::kNone;
    if (coord_field != 0U) {
        if (version == 2U) {
            coord_system = CoordinateSystem::kGeographic;
        } else if (coord_field <= static_cast<std::uint32_t>(CoordinateSystem::kGeographic)) {
            coord_system = static_cast<CoordinateSystem>(coord_field);
        } else {
            throw GraphIoError("graph file declares unknown coordinate system " +
                               std::to_string(coord_field));
        }
    }
    const bool has_coordinates = coord_system != CoordinateSystem::kNone;
    const std::uint64_t stored_checksum = detail::get_u64_le(is);

    // Every array below is sized from the header, so the header is checked
    // against the bytes actually present BEFORE anything is allocated. Trusting
    // it let a 72-byte file declaring 654 million nodes request 5 GB
    // (fuzz_graph_binary), and an arc count near 2^64 escape as std::length_error
    // instead of a GraphIoError. node_count <= 2^32 keeps the node term below
    // 2^38; the arc term is compared by division so it cannot overflow.
    const std::uint64_t node_bytes =
        (node_count + 1U) * 8U + node_count * ((has_mapping ? 8U : 0U) + (has_coordinates ? 16U : 0U));
    const std::optional<std::uint64_t> left = detail::remaining_bytes(is);
    if (left && (node_bytes > *left ||
                 arc_count > (*left - node_bytes) / detail::arc_record_bytes<W>())) {
        throw GraphIoError("graph file declares " + std::to_string(node_count) + " nodes and " +
                           std::to_string(arc_count) + " arcs, more than its remaining " +
                           std::to_string(*left) + " bytes can hold: it is truncated or corrupt");
    }
    // A stream that cannot report its size (a pipe) grows its arrays only as
    // bytes actually arrive, so a lying header fails at the first short read.
    const auto reservation = [&left](std::uint64_t declared) {
        constexpr std::uint64_t kUnverifiedCap = std::uint64_t{1} << 20;
        return static_cast<std::size_t>(left ? declared : std::min(declared, kUnverifiedCap));
    };

    std::vector<std::uint64_t> offsets;
    offsets.reserve(reservation(node_count + 1U));
    for (std::uint64_t i = 0; i <= node_count; ++i) {
        offsets.push_back(detail::get_u64_le(is));
    }
    if (offsets.back() != arc_count) {
        throw GraphIoError("graph file's final offset disagrees with its declared arc count");
    }
    if (offsets.front() != 0U ||
        std::adjacent_find(offsets.begin(), offsets.end(),
                           [](std::uint64_t a, std::uint64_t b) { return b < a; }) != offsets.end()) {
        throw GraphIoError("graph file's offsets do not start at 0 and increase monotonically");
    }

    // Rebuilt through the ordinary builder rather than by assigning the arrays
    // directly, so a file cannot install a CSR structure that the builder's
    // validation would have rejected.
    std::vector<InputEdge<W>> edges;
    edges.reserve(reservation(arc_count));
    for (std::uint64_t u = 0; u + 1 < offsets.size(); ++u) {
        for (std::uint64_t i = offsets[u]; i < offsets[u + 1]; ++i) {
            const NodeId target = detail::get_u32_le(is);
            const W      weight = detail::get_weight<W>(is);
            edges.push_back(InputEdge<W>{static_cast<NodeId>(u), target, weight});
        }
    }

    LoadedGraph<W> loaded;
    if (has_mapping) {
        loaded.mapping.original_id.reserve(reservation(node_count));
        for (std::uint64_t u = 0; u < node_count; ++u) {
            loaded.mapping.original_id.push_back(detail::get_u64_le(is));
        }
    }
    if (has_coordinates) {
        loaded.coordinates.reserve(reservation(node_count));
        for (std::uint64_t u = 0; u < node_count; ++u) {
            Coordinate c;
            c.x = detail::get_f64_le(is);
            c.y = detail::get_f64_le(is);
            loaded.coordinates.push_back(c);
        }
    }

    GraphBuildOptions opts;
    opts.directedness =
        undirected ? Directedness::kUndirected : Directedness::kDirected;
    opts.arcs_already_paired = true;  // both directions are already in the file
    opts.parallel_edges      = ParallelEdgePolicy::kKeep;
    opts.self_loops          = SelfLoopPolicy::kKeep;
    // Without this the coordinates load into the array but the graph reports
    // has_coordinates() == false, because that asks for a system as well.
    opts.coord_system        = coord_system;
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
                             const NodeIdMapping&           mapping      = {},
                             const std::vector<Coordinate>& coordinates  = {},
                             CoordinateSystem coord_system = CoordinateSystem::kNone) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        throw GraphIoError("cannot open '" + path + "' for writing");
    }
    write_binary_graph(os, g, mapping, coordinates, coord_system);
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

    /// Set when the file already lists BOTH directions of every undirected
    /// edge, so the reader must not pair them a second time.
    ///
    /// SNAP is not consistent about this and the file gives no indication
    /// either way: `facebook_combined.txt` lists each edge once (88,234
    /// records for 88,234 published edges), while `roadNet-PA.txt` lists both
    /// (3,083,796 records for 1,541,898 published edges). Reading the second
    /// as though it were the first produces a graph with twice the edges,
    /// every one of them duplicated -- which is perfectly valid, runs
    /// normally, and inflates every per-edge measurement taken on it.
    ///
    /// There is no way to infer this safely: a genuinely directed graph that
    /// happens to be symmetric is indistinguishable from a doubly-listed
    /// undirected one. So it is declared, and the dataset validator counts
    /// reciprocal records to check the declaration against the file.
    bool edges_listed_both_directions = false;
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
    opts.directedness        = options.directedness;
    opts.self_loops          = options.self_loops;
    opts.parallel_edges      = options.parallel_edges;
    opts.arcs_already_paired = options.edges_listed_both_directions;

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
