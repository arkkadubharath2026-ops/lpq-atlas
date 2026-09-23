// =============================================================================
// lpq_collect -- builds the offline training set (ESD.md section 39 milestone 15).
//
//   lpq_collect --dataset datasets/processed/dimacs_ny.lpqgraph
//               --queries-file experiments/manifests/dimacs_ny_queries_100.yaml
//               --output models/training/dimacs_ny.lpqtrain
//
// Runs the TRUSTED reference search over the query set and derives, per node,
// how often that node was the first entry extracted from its bucket. See
// training_data.hpp for what the label means and why the rows are aggregated.
//
// -----------------------------------------------------------------------------
// Why this uses the reference search and not the learned queue
// -----------------------------------------------------------------------------
//
// The labels must not come from the component being trained. If the learned
// queue produced them, the model would be fitted to the queue's current
// behaviour -- including its mistakes -- and improving the fit would mean
// reproducing them more faithfully. A reference Dijkstra settles nodes in
// nondecreasing distance order by construction, which is exactly the order the
// substrate must extract them in, so it supplies ground truth that owes nothing
// to the learned machinery.
//
// It also means this tool needs no instrumentation hooks in the queue at all.
// The hot path is untouched.
// =============================================================================
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/error.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/graph/graph_io.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/graph/query_manifest.hpp"
#include "lpq/models/training_data.hpp"

namespace lpq {
namespace {

using W     = std::uint32_t;
using Dist  = DistanceType<W>;
using Graph = CsrGraph<W>;

struct Options {
    std::string   dataset;
    std::string   queries_file;
    std::string   output;
    std::uint64_t bucket_width = 0;  ///< 0 derives it exactly as the runner does
};

[[noreturn]] void usage_and_exit(int code) {
    std::cout <<
        "lpq_collect -- build an offline training set from the reference search\n\n"
        "  --dataset <file.lpqgraph>   converted graph (required)\n"
        "  --queries-file <file.yaml>  query manifest (required)\n"
        "  --output <file.lpqtrain>    training set to write (required)\n"
        "  --bucket-width <n>          label buckets (default: derived as the runner does)\n";
    std::exit(code);
}

[[nodiscard]] Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                throw InvalidConfigError(std::string(what) + " requires a value");
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage_and_exit(0);
        } else if (a == "--dataset") {
            o.dataset = next("--dataset");
        } else if (a == "--queries-file") {
            o.queries_file = next("--queries-file");
        } else if (a == "--output") {
            o.output = next("--output");
        } else if (a == "--bucket-width") {
            o.bucket_width = std::stoull(next("--bucket-width"));
        } else {
            throw InvalidConfigError("unknown argument: " + a);
        }
    }
    if (o.dataset.empty() || o.queries_file.empty() || o.output.empty()) {
        throw InvalidConfigError("--dataset, --queries-file and --output are all required");
    }
    return o;
}

/// The runner's derivation, reproduced so the labels describe the geometry the
/// benchmark will actually run under.
///
/// A training set built at one bucket width and used at another is not wrong in
/// any way a program can detect -- it simply answers a different question than
/// the one the queue asks. The width is therefore recorded in the file, and
/// anything that loads it can check.
[[nodiscard]] std::uint64_t derive_bucket_width(const GraphMetadata& meta) {
    constexpr std::uint64_t kBuckets = 1024;  // BucketSubstrate::kBucketCount
    const auto span = static_cast<std::uint64_t>(meta.max_weight > 0.0 ? meta.max_weight : 1.0);
    const std::uint64_t width = (span + kBuckets - 1U) / kBuckets;
    return width == 0 ? 1 : width;
}

int run(int argc, char** argv) {
    const Options o = parse_args(argc, argv);

    LoadedGraph<W>      loaded = read_binary_graph_file<W>(o.dataset);
    Graph               g      = std::move(loaded.graph);
    const GraphMetadata meta   = compute_metadata(g);

    const QueryManifest manifest = read_query_manifest_file(o.queries_file);
    manifest.validate_against(g);

    const std::uint64_t width =
        o.bucket_width != 0 ? o.bucket_width : derive_bucket_width(meta);

    std::cout << "dataset      : " << std::filesystem::path(o.dataset).stem().string() << "\n"
              << "nodes / arcs : " << g.num_nodes() << " / " << g.num_arcs() << "\n"
              << "queries      : " << manifest.queries.size() << "\n"
              << "bucket width : " << width
              << (o.bucket_width != 0 ? "  (given)" : "  (derived)") << "\n";

    // --- static features, computed once -------------------------------------
    const GraphFeatures features(g, meta);

    TrainingSet set;
    set.dataset_id     = meta.dataset_id.empty()
                             ? std::filesystem::path(o.dataset).stem().string()
                             : meta.dataset_id;
    set.graph_checksum = graph_checksum<W>(g);
    set.bucket_width   = width;
    set.query_count    = manifest.queries.size();
    set.query_seed     = manifest.seed;

    set.rows.assign(g.num_nodes(), TrainingRow{});
    for (std::size_t u = 0; u < g.num_nodes(); ++u) {
        const NodeStaticFeatures& sf = features.node(static_cast<NodeId>(u));
        FeatureVector&            x  = set.rows[u].features;

        // Only the static group is filled, and the rest stay zero. That is not
        // an approximation of the dynamic features -- it is a statement that
        // this model does not see them, matching StaticNodeScorer exactly.
        set_feature(x, Feature::kOutDegree, sf.out_degree);
        set_feature(x, Feature::kMinAdjacentWeight, sf.min_adjacent_weight);
        set_feature(x, Feature::kMeanAdjacentWeight, sf.mean_adjacent_weight);
        set_feature(x, Feature::kMaxAdjacentWeight, sf.max_adjacent_weight);
        set_feature(x, Feature::kAdjacentWeightSpread, sf.weight_spread);
        set_feature(x, Feature::kDegreeRelativeToMean, sf.degree_relative_to_mean);
    }

    // --- labels, one full search per query -----------------------------------
    //
    // Full single-source, not single-target: a target-limited search settles
    // only part of the graph, and the nodes it stops short of would be recorded
    // as "never observed" rather than "not reached in this query". Those are
    // different, and the aggregate keeps them apart.
    std::vector<std::uint32_t> first_in_bucket;  // reused across queries

    std::size_t done = 0;
    for (const ManifestQuery& q : manifest.queries) {
        // reference_dijkstra_heap, not reference_dijkstra. The latter is the
        // O(n^2) oracle, and on a 264,346-node road network that is roughly
        // 7e10 operations for ONE query -- a run that never finishes rather
        // than one that is merely slow. Both are trusted; the heap variant is
        // the one reference_dijkstra.hpp names for graphs of this size.
        const auto result = reference_dijkstra_heap<W>(g, q.source);

        // The settle order is nondecreasing in distance, with ties broken by
        // node id, which is what makes "first out of its bucket" well defined
        // and reproducible. Rather than sort, the minimum per bucket is taken
        // directly: a node wins its bucket if no settled node shares that
        // bucket with a strictly smaller (distance, id).
        first_in_bucket.clear();

        // bucket -> winning node, kept in a flat map keyed by bucket index.
        // Buckets are unbounded here (a whole SSSP spans the graph's diameter),
        // so this is a hash-free two-pass over settled nodes instead.
        std::vector<std::pair<std::uint64_t, NodeId>> settled;
        settled.reserve(g.num_nodes());
        for (std::size_t v = 0; v < g.num_nodes(); ++v) {
            if (!result.settled[v]) {
                continue;
            }
            const auto d = static_cast<std::uint64_t>(result.dist[v]);
            settled.emplace_back(d / width, static_cast<NodeId>(v));
            ++set.rows[v].occurrences;
        }

        // Sort by (bucket, distance, id). Sorting by bucket then distance is
        // not the same as sorting by distance alone only when two nodes share a
        // bucket, which is exactly the case being decided.
        std::sort(settled.begin(), settled.end(),
                  [&result](const auto& a, const auto& b) {
                      if (a.first != b.first) {
                          return a.first < b.first;
                      }
                      const auto da = result.dist[a.second];
                      const auto db = result.dist[b.second];
                      if (da != db) {
                          return da < db;
                      }
                      return a.second < b.second;
                  });

        for (std::size_t i = 0; i < settled.size(); ++i) {
            if (i == 0 || settled[i].first != settled[i - 1].first) {
                ++set.rows[settled[i].second].positives;
            }
        }

        if (++done % 25 == 0) {
            std::cout << "  " << done << " / " << manifest.queries.size() << " queries\n";
        }
    }

    // --- report the imbalance before anything trains on this -----------------
    const std::uint64_t positives   = set.total_positives();
    const std::uint64_t occurrences = set.total_occurrences();
    const double        rate =
        occurrences == 0 ? 0.0
                                : static_cast<double>(positives) / static_cast<double>(occurrences);

    std::cout << "\nrows          : " << set.rows.size() << " (one per node)\n"
              << "observations  : " << occurrences << "\n"
              << "positives     : " << positives << "\n"
              << "positive rate : " << rate << "\n"
              << "\n  A model that always answers \"no\" scores "
              << (1.0 - rate) * 100.0
              << "% accuracy on this set.\n"
                 "  Any reported accuracy must be read against that number.\n";

    const std::filesystem::path path(o.output);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    write_training_set_file(o.output, set);
    std::cout << "\nwrote " << o.output << "\n";
    return 0;
}

}  // namespace
}  // namespace lpq

int main(int argc, char** argv) {
    try {
        return lpq::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
