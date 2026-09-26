// =============================================================================
// lpq_export -- expose a converted graph's arrays to the web layer.
//
// The orchestration API needs node coordinates (map rendering, click
// snapping) and adjacency (drawing the road network in a viewport). It must not
// get them from a second parser of the .lpqgraph format: graph_io.hpp owns that
// format, and binary_header.py already explains why Python reads only the
// header. So the C++ reader loads the graph and this tool writes flat,
// self-describing little-endian arrays that numpy maps directly:
//
//   coords.f64    n x 2 doubles (x, y) -- (lon, lat) for geographic graphs
//   offsets.u64   n + 1 CSR row offsets
//   targets.u32   arc targets
//   weights.u32   arc weights
//   original_ids.u64  the dataset's own node ids, when the file stores them
//   export.json   counts, checksum and the file list
//
// Offline preparation, never timed.
// =============================================================================
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lpq/graph/graph_io.hpp"
#include "lpq/metrics/json_writer.hpp"

namespace {

using W = std::uint32_t;

template <typename T>
void write_array(const std::filesystem::path& p, const std::vector<T>& v) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(v.data()),
              static_cast<std::streamsize>(v.size() * sizeof(T)));
    if (!out) {
        throw std::runtime_error("cannot write " + p.filename().string());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataset;
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dataset" && i + 1 < argc) {
            dataset = argv[++i];
        } else if (a == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else {
            std::cerr << "usage: lpq_export --dataset <file.lpqgraph> --out-dir <dir>\n";
            return 64;
        }
    }
    if (dataset.empty() || out_dir.empty()) {
        std::cerr << "usage: lpq_export --dataset <file.lpqgraph> --out-dir <dir>\n";
        return 64;
    }
    try {
        const auto  loaded = lpq::read_binary_graph_file<W>(dataset);
        const auto& g      = loaded.graph;
        const std::filesystem::path dir(out_dir);
        std::filesystem::create_directories(dir);

        const std::size_t          n = g.num_nodes();
        std::vector<std::uint64_t> offsets(n + 1, 0);
        std::vector<std::uint32_t> targets;
        std::vector<std::uint32_t> weights;
        targets.reserve(g.num_arcs());
        weights.reserve(g.num_arcs());
        for (lpq::NodeId u = 0; u < n; ++u) {
            for (const auto& a : g.out_arcs(u)) {
                targets.push_back(a.target);
                weights.push_back(a.weight);
            }
            offsets[u + 1] = targets.size();
        }
        write_array(dir / "offsets.u64", offsets);
        write_array(dir / "targets.u32", targets);
        write_array(dir / "weights.u32", weights);

        const bool coords = g.has_coordinates();
        if (coords) {
            std::vector<double> xy;
            xy.reserve(2 * n);
            for (const auto& c : g.coordinates()) {
                xy.push_back(c.x);
                xy.push_back(c.y);
            }
            write_array(dir / "coords.f64", xy);
        }
        const bool mapping = !loaded.mapping.empty();
        if (mapping) {
            write_array(dir / "original_ids.u64", loaded.mapping.original_id);
        }

        std::ofstream   meta(dir / "export.json", std::ios::trunc);
        lpq::JsonWriter w(meta, true);
        w.begin_object();
        w.key_value("export_version", static_cast<std::int64_t>(1));
        w.key_value("nodes", static_cast<std::uint64_t>(n));
        w.key_value("arcs", static_cast<std::uint64_t>(targets.size()));
        w.key_value("checksum_fnv1a64", std::to_string(loaded.checksum));
        w.key_value("directed", g.directedness() == lpq::Directedness::kDirected);
        w.key_value("has_coordinates", coords);
        w.key_value("coordinate_system",
                    g.coordinate_system() == lpq::CoordinateSystem::kGeographic   ? "geographic"
                    : g.coordinate_system() == lpq::CoordinateSystem::kEuclidean ? "euclidean"
                                                                                  : "none");
        w.key_value("has_original_ids", mapping);
        w.end_object();
        meta << '\n';
        return meta ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
