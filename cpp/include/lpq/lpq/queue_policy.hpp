// =============================================================================
// Cost-aware queue-configuration policy (the "optimised" queue's decision).
//
// What this decides and, deliberately, what it does not:
//
//   * It chooses a QUEUE GEOMETRY once per graph, before any search: radix heap,
//     or a pooled bucket queue with a particular bucket width. It never touches
//     a search in flight, so there is no mode switch that could leave a queue in
//     an invalid state, and the hot path pays nothing for the decision.
//   * Every candidate is an exact queue. The policy can pick a slow one; it
//     cannot pick a wrong one.
//   * When the chosen queue is the radix heap, the result is a classical heap's
//     speed and must be reported as such, not as a gain of the learned queue.
//
// The decision is a tiny linear model per candidate, trained offline by
// python/lpq_tools/policy on measured paired runtimes and validated on graphs
// held out of training. Each candidate predicts log(time / time of radix heap)
// from a handful of graph-level weight statistics. The candidate with the
// lowest predicted ratio wins if its prediction beats radix by more than the
// policy's `margin`; otherwise radix is kept. The margin is the cost-aware part:
// it prices model error and the work of computing the profile, so a candidate
// must be predicted to win clearly before the safe classical choice is left.
//
// With no policy file the policy is "always radix", which is the strongest
// classical baseline: an absent model degrades to the baseline, never below it.
// =============================================================================
#ifndef LPQ_LPQ_QUEUE_POLICY_HPP
#define LPQ_LPQ_QUEUE_POLICY_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq/graph/graph_metadata.hpp"

namespace lpq {

/// Graph-level statistics the policy reads. Computed once per graph.
struct WeightProfile {
    std::uint64_t nodes = 0;
    std::uint64_t arcs  = 0;
    double        min_weight = 0.0;
    double        max_weight = 0.0;
    double        mean_weight = 0.0;
    double        stddev_weight = 0.0;
    double        p50 = 0.0;
    double        p90 = 0.0;
    double        p99 = 0.0;
    double        mean_degree = 0.0;

    /// True when the quantiles come from a deterministic stride sample rather
    /// than every arc (graphs beyond kExactQuantileArcs).
    bool          quantiles_sampled = false;

    /// Wall time to compute this profile. Part of the optimised queue's cost
    /// and reported with it; it is paid once per graph, not per query.
    std::uint64_t compute_ns = 0;
};

inline constexpr std::size_t kExactQuantileArcs = std::size_t{1} << 23;

template <WeightType W>
[[nodiscard]] WeightProfile compute_weight_profile(const CsrGraph<W>& g, const GraphMetadata& meta) {
    const auto t0 = std::chrono::steady_clock::now();

    WeightProfile p;
    p.nodes         = meta.node_count;
    p.arcs          = meta.arc_count;
    p.min_weight    = meta.min_weight;
    p.max_weight    = meta.max_weight;
    p.mean_weight   = meta.mean_weight;
    p.stddev_weight = meta.stddev_weight;
    p.mean_degree   = meta.density();

    const auto arcs = g.arcs();
    if (!arcs.empty()) {
        // Deterministic stride sample for very large graphs: the quantiles only
        // feed a coarse decision, and a full copy-and-select of tens of millions
        // of arcs would cost more than it is worth.
        const std::size_t stride =
            arcs.size() > kExactQuantileArcs ? (arcs.size() / kExactQuantileArcs) + 1 : 1;
        p.quantiles_sampled = stride > 1;

        std::vector<W> sample;
        sample.reserve(arcs.size() / stride + 1);
        for (std::size_t i = 0; i < arcs.size(); i += stride) {
            sample.push_back(arcs[i].weight);
        }
        const auto quantile = [&sample](double q) {
            const std::size_t k = std::min(sample.size() - 1,
                                           static_cast<std::size_t>(q * static_cast<double>(sample.size())));
            std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(k), sample.end());
            return static_cast<double>(sample[k]);
        };
        p.p50 = quantile(0.50);
        p.p90 = quantile(0.90);
        p.p99 = quantile(0.99);
    }

    p.compute_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    return p;
}


// -----------------------------------------------------------------------------
// Per-graph profile cache
// -----------------------------------------------------------------------------
//
// Computing a profile is O(arcs) and, on a 700k-arc road network, costs about as
// much as one query. That is a one-time property of the GRAPH, like converting
// it, so it is cached beside the graph file, keyed by the graph's checksum. A
// stale or foreign cache can never be used: the checksum must match.
//
// What is reported alongside every use is the truth about it: `source` says
// whether the profile was computed in this process or read from the cache, and
// `compute_ns` is the original computation cost either way, so nobody can read a
// per-query time that quietly excludes work that was in fact just done.

enum class ProfileSource : std::uint8_t { kComputed, kComputedAndCached, kCached };

[[nodiscard]] inline const char* to_string(ProfileSource s) noexcept {
    switch (s) {
        case ProfileSource::kComputed: return "computed";
        case ProfileSource::kComputedAndCached: return "computed_and_cached";
        case ProfileSource::kCached: return "cached";
    }
    return "unknown";
}

template <WeightType W>
[[nodiscard]] WeightProfile load_or_compute_profile(const std::string& cache_path,
                                                    std::uint64_t graph_checksum,
                                                    const CsrGraph<W>& g, const GraphMetadata& meta,
                                                    ProfileSource& source) {
    if (!cache_path.empty()) {
        std::ifstream in(cache_path);
        if (in) {
            WeightProfile p;
            std::uint64_t checksum = 0;
            int           version  = 0;
            std::string   key;
            std::uint64_t quantiles_sampled = 0;
            bool          ok = true;
            while (in >> key) {
                if (key == "lpqprofile") in >> version;
                else if (key == "checksum") in >> checksum;
                else if (key == "nodes") in >> p.nodes;
                else if (key == "arcs") in >> p.arcs;
                else if (key == "min_weight") in >> p.min_weight;
                else if (key == "max_weight") in >> p.max_weight;
                else if (key == "mean_weight") in >> p.mean_weight;
                else if (key == "stddev_weight") in >> p.stddev_weight;
                else if (key == "p50") in >> p.p50;
                else if (key == "p90") in >> p.p90;
                else if (key == "p99") in >> p.p99;
                else if (key == "mean_degree") in >> p.mean_degree;
                else if (key == "quantiles_sampled") in >> quantiles_sampled;
                else if (key == "compute_ns") in >> p.compute_ns;
                else { ok = false; break; }
            }
            if (ok && version == 1 && checksum == graph_checksum && p.nodes == meta.node_count &&
                p.arcs == meta.arc_count) {
                p.quantiles_sampled = quantiles_sampled != 0;
                source              = ProfileSource::kCached;
                return p;
            }
        }
    }
    WeightProfile p = compute_weight_profile(g, meta);
    source          = ProfileSource::kComputed;
    if (!cache_path.empty()) {
        // Written to a temporary and renamed so a concurrent reader never sees a
        // half-written profile. A failure to write is not an error: the cache is
        // an optimisation and the result already carries source=computed.
        const std::string tmp = cache_path + ".tmp";
        std::ofstream     out(tmp);
        if (out) {
            out.precision(17);
            out << "lpqprofile 1\nchecksum " << graph_checksum << "\nnodes " << p.nodes
                << "\narcs " << p.arcs << "\nmin_weight " << p.min_weight << "\nmax_weight "
                << p.max_weight << "\nmean_weight " << p.mean_weight << "\nstddev_weight "
                << p.stddev_weight << "\np50 " << p.p50 << "\np90 " << p.p90 << "\np99 "
                << p.p99 << "\nmean_degree " << p.mean_degree << "\nquantiles_sampled "
                << (p.quantiles_sampled ? 1 : 0) << "\ncompute_ns " << p.compute_ns << "\n";
            out.close();
            if (out && std::rename(tmp.c_str(), cache_path.c_str()) == 0) {
                source = ProfileSource::kComputedAndCached;
            }
        }
    }
    return p;
}

/// The policy's input vector. The names are written into the policy file and
/// checked on load, so a model trained on one feature list can never be
/// silently evaluated on another.
inline constexpr std::size_t kPolicyFeatureCount = 7;
inline constexpr std::array<const char*, kPolicyFeatureCount> kPolicyFeatureNames = {
    "log2_max_w", "log2_mean_w", "p50_over_max", "p99_over_max",
    "cv_w",       "log2_nodes",  "mean_degree"};

[[nodiscard]] inline std::array<double, kPolicyFeatureCount> policy_features(const WeightProfile& p) {
    const double mx   = std::max(1.0, p.max_weight);
    const double mean = std::max(1e-9, p.mean_weight);
    return {std::log2(mx + 1.0),
            std::log2(mean + 1.0),
            p.p50 / mx,
            p.p99 / mx,
            p.stddev_weight / mean,
            std::log2(static_cast<double>(std::max<std::uint64_t>(1, p.nodes))),
            p.mean_degree};
}

enum class QueueKind : std::uint8_t { kRadixHeap = 0, kPooledBucket = 1 };

/// What the policy decided, and why, for reporting.
struct QueueChoice {
    QueueKind     kind  = QueueKind::kRadixHeap;
    std::uint64_t width = 1;       ///< pooled bucket width
    std::size_t   ring  = 0;       ///< pooled ring size (power of two)
    double        predicted_ratio = 1.0;  ///< predicted time / radix time of the chosen queue
    std::string   reason;

    [[nodiscard]] std::string describe() const {
        if (kind == QueueKind::kRadixHeap) {
            return "radix_heap";
        }
        return "pooled_bucket(width=" + std::to_string(width) + ",ring=" + std::to_string(ring) + ")";
    }
};

/// The smallest power-of-two ring that holds every key a push can produce.
///
/// A pushed key is at most (current bucket's top) + max_step, so its bucket
/// offset is at most 1 + max_step / width. `max_step` is the largest amount a
/// key can rise in one relaxation: the maximum arc weight for Dijkstra, and up
/// to twice that for A* with a consistent heuristic (w + h(v) - h(u) <= 2w).
[[nodiscard]] inline std::size_t pooled_ring_for(std::uint64_t max_step, std::uint64_t width) {
    const std::uint64_t need = max_step / width + 3U;
    return std::bit_ceil(static_cast<std::size_t>(need));
}

class QueueSelectionPolicy {
public:
    /// Largest ring the policy will ask for. 2^18 head slots is 1 MiB, which
    /// still sits in a typical L2; beyond it narrow buckets stop being cheap.
    static constexpr std::size_t kMaxPolicyRing = std::size_t{1} << 18;

    struct Candidate {
        std::uint64_t                              width = 1;
        double                                     intercept = 0.0;
        std::array<double, kPolicyFeatureCount>    coef{};
    };

    /// "Always radix": what an absent policy means.
    QueueSelectionPolicy() = default;

    [[nodiscard]] static QueueSelectionPolicy load(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            throw InvalidConfigError("cannot open queue policy: " + path);
        }
        QueueSelectionPolicy pol;
        pol.source_ = path;
        std::string line;
        bool        header = false;
        bool        features_checked = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream ls(line);
            std::string        key;
            ls >> key;
            if (key == "lpqpolicy") {
                int version = 0;
                ls >> version;
                if (version != 1) {
                    throw InvalidConfigError("unsupported queue policy version in " + path);
                }
                header = true;
            } else if (key == "margin") {
                ls >> pol.margin_;
            } else if (key == "trained_on") {
                std::getline(ls, pol.trained_on_);
            } else if (key == "features") {
                std::vector<std::string> names;
                for (std::string n; ls >> n;) {
                    names.push_back(n);
                }
                if (names.size() != kPolicyFeatureCount ||
                    !std::equal(names.begin(), names.end(), kPolicyFeatureNames.begin(),
                                [](const std::string& a, const char* b) { return a == b; })) {
                    throw InvalidConfigError(
                        "queue policy " + path + " was trained on a different feature list "
                        "than this engine computes; retrain it");
                }
                features_checked = true;
            } else if (key == "candidate") {
                Candidate c;
                std::string tok;
                ls >> tok;  // kind; only "pooled" exists
                if (tok != "pooled") {
                    throw InvalidConfigError("unknown candidate kind '" + tok + "' in " + path);
                }
                ls >> tok;
                if (tok.rfind("w=", 0) != 0) {
                    throw InvalidConfigError("candidate needs w=<width> in " + path);
                }
                c.width = std::stoull(tok.substr(2));
                ls >> tok;
                if (tok != "intercept") {
                    throw InvalidConfigError("candidate needs intercept in " + path);
                }
                ls >> c.intercept;
                ls >> tok;
                if (tok != "coef") {
                    throw InvalidConfigError("candidate needs coef in " + path);
                }
                for (double& v : c.coef) {
                    if (!(ls >> v)) {
                        throw InvalidConfigError("candidate has too few coefficients in " + path);
                    }
                }
                if (c.width == 0) {
                    throw InvalidConfigError("candidate width must be >= 1 in " + path);
                }
                pol.candidates_.push_back(c);
            } else {
                throw InvalidConfigError("unknown queue policy key '" + key + "' in " + path);
            }
        }
        if (!header || !features_checked) {
            throw InvalidConfigError("queue policy " + path + " is missing its header or features");
        }
        return pol;
    }

    /// Chooses a queue for a graph. `max_step` is the largest key rise one
    /// relaxation can produce (see pooled_ring_for).
    [[nodiscard]] QueueChoice choose(const WeightProfile& profile, std::uint64_t max_step) const {
        QueueChoice best;  // radix, ratio 1.0
        if (candidates_.empty()) {
            best.reason = source_.empty() ? "no policy loaded: radix heap is the default"
                                          : "policy lists no candidates: radix heap";
            return best;
        }
        const auto x = policy_features(profile);
        double     best_ratio = 1.0 - margin_;
        bool       found      = false;
        for (const Candidate& c : candidates_) {
            const std::size_t ring = pooled_ring_for(max_step, c.width);
            if (ring > kMaxPolicyRing) {
                continue;  // this geometry would not fit a cache-resident ring
            }
            double z = c.intercept;
            for (std::size_t i = 0; i < kPolicyFeatureCount; ++i) {
                z += c.coef[i] * x[i];
            }
            const double ratio = std::exp(z);
            if (ratio < best_ratio) {
                best_ratio       = ratio;
                best.kind        = QueueKind::kPooledBucket;
                best.width       = c.width;
                best.ring        = ring;
                best.predicted_ratio = ratio;
                found            = true;
            }
        }
        if (!found) {
            best.reason = "no candidate predicted to beat radix_heap by the required margin";
        } else {
            best.reason = "predicted " + std::to_string(best.predicted_ratio) +
                          "x the radix heap's time (margin " + std::to_string(margin_) + ")";
        }
        return best;
    }

    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] const std::string& trained_on() const noexcept { return trained_on_; }
    [[nodiscard]] double             margin() const noexcept { return margin_; }
    [[nodiscard]] std::size_t        candidate_count() const noexcept { return candidates_.size(); }

private:
    std::vector<Candidate> candidates_;
    double                 margin_ = 0.03;
    std::string            source_;
    std::string            trained_on_;
};

}  // namespace lpq

#endif  // LPQ_LPQ_QUEUE_POLICY_HPP
