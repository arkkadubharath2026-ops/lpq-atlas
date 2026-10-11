// =============================================================================
// Per-graph, per-machine queue autotuning for lpq_optimized.
//
// Why this exists instead of a predictor. A policy trained on graph statistics
// was evaluated leave-one-TOPOLOGY-out on 17 graphs (docs/experiments/
// queue-policy.md): it never beat the radix heap on a held-out graph, because
// which queue wins depends on cache behaviour and key density that six
// topologies cannot teach a model. The measured gains are real but local (the
// pooled bucket queue is ~10% faster than radix on DIMACS New York and ties or
// loses on most others), and they depend on the CPU the user runs on.
//
// So the decision is measured, on the graph and the machine in front of us:
//
//   1. draw a small deterministic sample of source/target pairs;
//   2. for each pair run every candidate queue back to back, in a shuffled
//      order (so slow drift hits all of them equally), and keep the best of a
//      few repetitions;
//   3. take the geometric mean of time(candidate) / time(radix heap) over the
//      pairs, and the share of pairs the candidate won;
//   4. use a pooled queue only if it beats radix by a clear margin AND wins on
//      most pairs; otherwise keep the radix heap, the strongest classical
//      baseline.
//
// Everything is exact: every candidate is a correct queue, so a bad choice is
// slow, never wrong. The calibration cost is measured and reported with the
// decision, and the decision is cached beside the graph keyed by the graph's
// checksum and the machine, so the cost is paid once.
// =============================================================================
#ifndef LPQ_LPQ_QUEUE_AUTOTUNE_HPP
#define LPQ_LPQ_QUEUE_AUTOTUNE_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/metrics/benchmark_harness.hpp"
#include "lpq/lpq/pooled_bucket_queue.hpp"
#include "lpq/lpq/queue_policy.hpp"
#include "lpq/pq/radix_heap.hpp"

namespace lpq {

struct AutotuneConfig {
    std::uint32_t sample_queries = 12;   ///< source/target pairs timed
    std::uint32_t repetitions    = 3;    ///< per candidate per pair; the best is kept
    double        required_ratio = 0.97; ///< geometric-mean ratio vs radix must be below this
    double        required_wins  = 0.60; ///< share of pairs the candidate must win
    std::uint64_t seed           = 20260923;
    /// Pooled widths to try. Infeasible ones (ring beyond the cap) are skipped.
    std::vector<std::uint64_t> widths = {1, 2, 4};
};

struct AutotuneEntry {
    std::uint64_t width = 0;      ///< 0 = radix heap (the reference)
    double        geomean_ratio = 1.0;
    double        win_share = 0.0;
};

struct AutotuneResult {
    QueueChoice                choice;           ///< what to run
    std::vector<AutotuneEntry> measured;         ///< everything tried, nothing dropped
    std::uint32_t              queries = 0;
    std::uint64_t              calibration_ns = 0;
    bool                       from_cache = false;
    std::string                reason;
};

/// Identifies the machine well enough that a decision measured on another CPU is
/// never reused. Not a security boundary: a collision only costs a recalibration.
[[nodiscard]] inline std::uint64_t machine_key(const std::string& cpu_model, unsigned cores) {
    return std::hash<std::string>{}(cpu_model + "|" + std::to_string(cores));
}

template <WeightType W>
class QueueAutotuner {
public:
    using Dist = DistanceType<W>;

    explicit QueueAutotuner(AutotuneConfig cfg = {}) : cfg_(std::move(cfg)) {}

    [[nodiscard]] AutotuneResult tune(const CsrGraph<W>& g, const WeightProfile& profile,
                                      std::uint64_t max_step) const {
        const auto t_begin = std::chrono::steady_clock::now();
        AutotuneResult out;
        out.queries = cfg_.sample_queries;

        // Candidates: index 0 is always the radix heap (the reference).
        struct Cand {
            std::uint64_t width;
            std::size_t   ring;
            PooledBucketQueue<W> queue;
        };
        std::vector<Cand> cands;
        for (std::uint64_t w : cfg_.widths) {
            const std::size_t ring = pooled_ring_for(max_step, w);
            if (ring > QueueSelectionPolicy::kMaxPolicyRing) {
                continue;
            }
            cands.push_back(Cand{w, ring, PooledBucketQueue<W>(static_cast<Dist>(w), ring)});
        }
        if (cands.empty() || g.num_nodes() < 2) {
            out.reason = "no feasible pooled geometry for this graph: radix heap";
            return finish(out, t_begin);
        }

        RadixHeap<Dist> radix;
        radix.reserve_nodes(g.num_nodes());

        Rng rng(derive_seed(cfg_.seed, "autotune"));
        std::vector<double>              log_ratio_sum(cands.size(), 0.0);
        std::vector<std::uint32_t>       wins(cands.size(), 0);
        std::vector<std::size_t>         order(cands.size() + 1);
        std::vector<double>              best_ns(cands.size() + 1);

        for (std::uint32_t qi = 0; qi < cfg_.sample_queries; ++qi) {
            const NodeId s = rng.next_node(g.num_nodes());
            NodeId       t = rng.next_node(g.num_nodes());
            if (t == s) {
                t = static_cast<NodeId>((t + 1) % g.num_nodes());
            }
            std::iota(order.begin(), order.end(), std::size_t{0});
            for (std::size_t k = order.size(); k > 1; --k) {
                std::swap(order[k - 1], order[rng.next_below(k)]);
            }
            for (std::size_t idx : order) {
                best_ns[idx] = run_best(g, s, t, idx == 0 ? nullptr : &cands[idx - 1].queue, radix);
            }
            for (std::size_t c = 0; c < cands.size(); ++c) {
                const double ratio = best_ns[c + 1] / best_ns[0];
                log_ratio_sum[c] += std::log(ratio);
                wins[c] += ratio < 1.0 ? 1U : 0U;
            }
        }

        out.measured.push_back(AutotuneEntry{0, 1.0, 0.0});
        std::size_t best = cands.size();
        double      best_ratio = 1.0;
        for (std::size_t c = 0; c < cands.size(); ++c) {
            AutotuneEntry e;
            e.width         = cands[c].width;
            e.geomean_ratio = std::exp(log_ratio_sum[c] / cfg_.sample_queries);
            e.win_share     = static_cast<double>(wins[c]) / cfg_.sample_queries;
            out.measured.push_back(e);
            if (e.geomean_ratio < best_ratio) {
                best_ratio = e.geomean_ratio;
                best       = c;
            }
        }

        if (best < cands.size() && best_ratio <= cfg_.required_ratio &&
            out.measured[best + 1].win_share >= cfg_.required_wins) {
            out.choice.kind            = QueueKind::kPooledBucket;
            out.choice.width           = cands[best].width;
            out.choice.ring            = cands[best].ring;
            out.choice.predicted_ratio = best_ratio;
            out.reason = "measured " + std::to_string(best_ratio) + "x the radix heap's time on " +
                         std::to_string(cfg_.sample_queries) + " sampled queries";
        } else {
            out.reason = "no pooled geometry beat the radix heap by the required margin (" +
                         std::to_string(cfg_.required_ratio) + "x on at least " +
                         std::to_string(static_cast<int>(cfg_.required_wins * 100)) +
                         "% of sampled queries): radix heap";
        }
        (void)profile;
        return finish(out, t_begin);
    }

private:
    template <typename Q>
    static double time_once(const CsrGraph<W>& g, NodeId s, NodeId t, Q& q) {
        DijkstraOptions o;
        o.target       = t;
        o.measure_time = false;
        const auto t0  = std::chrono::steady_clock::now();
        const auto r   = dijkstra<W, Q>(g, s, q, o);
        const auto t1  = std::chrono::steady_clock::now();
        do_not_optimize(r.dist[t]);
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    double run_best(const CsrGraph<W>& g, NodeId s, NodeId t, PooledBucketQueue<W>* pooled,
                    RadixHeap<DistanceType<W>>& radix) const {
        double best = 1e300;
        // One warm-up run so the first candidate of a pair does not pay for the
        // cold caches of the whole pair.
        if (pooled != nullptr) (void)time_once(g, s, t, *pooled); else (void)time_once(g, s, t, radix);
        for (std::uint32_t r = 0; r < cfg_.repetitions; ++r) {
            const double ns = pooled != nullptr ? time_once(g, s, t, *pooled)
                                                : time_once(g, s, t, radix);
            best = std::min(best, ns);
        }
        return best;
    }

    static AutotuneResult finish(AutotuneResult out, std::chrono::steady_clock::time_point t0) {
        out.calibration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
                .count());
        return out;
    }

    AutotuneConfig cfg_;
};

// -----------------------------------------------------------------------------
// Cache: "<graph>.lpqtune", keyed by graph checksum, machine, algorithm and the
// calibration settings that affect the decision.
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::string autotune_cache_text(const AutotuneResult& r, std::uint64_t checksum,
                                                     std::uint64_t machine, const std::string& algorithm,
                                                     const AutotuneConfig& cfg) {
    std::ostringstream os;
    os.precision(17);
    os << "lpqtune 1\nchecksum " << checksum << "\nmachine " << machine << "\nalgorithm " << algorithm
       << "\nsample_queries " << cfg.sample_queries << "\nrepetitions " << cfg.repetitions
       << "\nrequired_ratio " << cfg.required_ratio << "\nrequired_wins " << cfg.required_wins
       << "\nseed " << cfg.seed << "\nkind " << static_cast<int>(r.choice.kind) << "\nwidth "
       << r.choice.width << "\nring " << r.choice.ring << "\nratio " << r.choice.predicted_ratio
       << "\ncalibration_ns " << r.calibration_ns << "\nreason " << r.reason << "\n";
    for (const auto& m : r.measured) {
        os << "measured " << m.width << " " << m.geomean_ratio << " " << m.win_share << "\n";
    }
    return os.str();
}

/// Reads a cache file; returns false (and leaves `out` untouched) unless it
/// matches the current graph, machine, algorithm and calibration settings.
[[nodiscard]] inline bool autotune_cache_load(const std::string& path, std::uint64_t checksum,
                                              std::uint64_t machine, const std::string& algorithm,
                                              const AutotuneConfig& cfg, AutotuneResult& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string        line;
    AutotuneResult     r;
    std::uint64_t      f_checksum = 0, f_machine = 0, f_seed = 0;
    std::uint32_t      f_q = 0, f_rep = 0;
    double             f_req = 0, f_wins = 0;
    std::string        f_alg;
    int                version = 0, kind = 0;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string        key;
        ls >> key;
        if (key == "lpqtune") ls >> version;
        else if (key == "checksum") ls >> f_checksum;
        else if (key == "machine") ls >> f_machine;
        else if (key == "algorithm") ls >> f_alg;
        else if (key == "sample_queries") ls >> f_q;
        else if (key == "repetitions") ls >> f_rep;
        else if (key == "required_ratio") ls >> f_req;
        else if (key == "required_wins") ls >> f_wins;
        else if (key == "seed") ls >> f_seed;
        else if (key == "kind") ls >> kind;
        else if (key == "width") ls >> r.choice.width;
        else if (key == "ring") ls >> r.choice.ring;
        else if (key == "ratio") ls >> r.choice.predicted_ratio;
        else if (key == "calibration_ns") ls >> r.calibration_ns;
        else if (key == "reason") { std::getline(ls, r.reason); if (!r.reason.empty() && r.reason[0] == ' ') r.reason.erase(0, 1); }
        else if (key == "measured") {
            AutotuneEntry e;
            ls >> e.width >> e.geomean_ratio >> e.win_share;
            r.measured.push_back(e);
        } else {
            return false;
        }
    }
    if (version != 1 || f_checksum != checksum || f_machine != machine || f_alg != algorithm ||
        f_q != cfg.sample_queries || f_rep != cfg.repetitions || f_req != cfg.required_ratio ||
        f_wins != cfg.required_wins || f_seed != cfg.seed) {
        return false;
    }
    r.choice.kind = kind == 1 ? QueueKind::kPooledBucket : QueueKind::kRadixHeap;
    r.queries     = f_q;
    r.from_cache  = true;
    out           = std::move(r);
    return true;
}

inline void autotune_cache_store(const std::string& path, const AutotuneResult& r,
                                 std::uint64_t checksum, std::uint64_t machine,
                                 const std::string& algorithm, const AutotuneConfig& cfg) {
    if (path.empty()) {
        return;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            return;  // the cache is an optimisation; the result already carries from_cache=false
        }
        out << autotune_cache_text(r, checksum, machine, algorithm, cfg);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
    }
}

}  // namespace lpq

#endif  // LPQ_LPQ_QUEUE_AUTOTUNE_HPP
