// Trace correctness (LPQ Atlas spec section 42).
//
// A visualiser is only as honest as its event stream. These tests pin the
// properties that make a replay trustworthy:
//
//   * observing a search does not change it -- distances, paths and every
//     operation counter are identical with and without the observer;
//   * sequence numbers are gap-free and strictly increasing;
//   * NODE_SETTLED events are exactly the engine's settles, and replaying
//     EDGE_RELAXED reproduces the engine's final distances;
//   * learned-queue decision events agree with the queue's own counters;
//   * a capped trace reports exactly how many events it dropped.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/algorithms/astar.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/heuristics.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/radix_heap.hpp"
#include "lpq/trace/trace_recorder.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using namespace lpq;
using W     = std::uint32_t;
using Dist  = DistanceType<W>;
using Graph = CsrGraph<W>;
using Lpq   = LearnedPriorityQueue<W, ConstantNodeScorer>;

struct Event {
    std::string                          type;
    std::uint64_t                        seq = 0;
    std::map<std::string, double>        num;
    std::string                          line;
};

/// Minimal field reader for the flat lines TraceLine writes. Only scalar
/// numeric fields are extracted; arrays are left in `line`.
std::vector<Event> parse(const std::string& jsonl) {
    std::vector<Event> out;
    std::istringstream in(jsonl);
    std::string        line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Event e;
        e.line = line;
        const auto t = line.find("\"type\":\"");
        e.type       = line.substr(t + 8, line.find('"', t + 8) - (t + 8));
        std::size_t pos = 0;
        while ((pos = line.find('"', pos)) != std::string::npos) {
            const auto end = line.find('"', pos + 1);
            if (end == std::string::npos || end + 1 >= line.size() || line[end + 1] != ':') {
                pos = end == std::string::npos ? end : end + 1;
                continue;
            }
            const std::string key = line.substr(pos + 1, end - pos - 1);
            const char        c   = line[end + 2];
            if ((c >= '0' && c <= '9') || c == '-') {
                e.num[key] = std::stod(line.substr(end + 2));
            }
            pos = end + 1;
        }
        e.seq = static_cast<std::uint64_t>(e.num["seq"]);
        out.push_back(std::move(e));
    }
    return out;
}

Graph random_connected_graph(std::uint64_t seed) {
    Rng                    rng(seed);
    lpq_test::RandomGraphSpec spec;
    spec.min_nodes        = 20;
    spec.max_nodes        = 60;
    spec.edge_probability = 0.12;
    spec.max_weight       = 50;
    return lpq_test::random_graph(rng, spec);
}

template <typename Queue>
Queue make(const Graph& g) {
    if constexpr (std::is_same_v<Queue, Lpq>) {
        (void)g;
        return Queue{static_cast<Dist>(4), CandidateWindowConfig{}, CorrectnessPolicy::exact()};
    } else {
        (void)g;
        return Queue{};
    }
}

template <typename Queue>
void check_observed_equals_unobserved(const Graph& g, NodeId s, NodeId t) {
    Queue q1 = make<Queue>(g);
    q1.reserve_nodes(g.num_nodes());
    DijkstraOptions o;
    o.target = t;
    const auto plain = dijkstra<W, Queue>(g, s, q1, o);

    std::ostringstream sink;
    TraceRecorder      rec(sink, TraceLevel::kFull, 1'000'000, false);
    Queue              q2 = make<Queue>(g);
    q2.reserve_nodes(g.num_nodes());
    const auto traced = dijkstra<W, Queue, TraceRecorder>(g, s, q2, o, &rec);

    ASSERT_EQ(plain.dist, traced.dist);
    ASSERT_EQ(plain.termination, traced.termination);
    ASSERT_EQ(plain.counters.settled_nodes, traced.counters.settled_nodes);
    ASSERT_EQ(plain.counters.pq_pops, traced.counters.pq_pops);
    ASSERT_EQ(plain.counters.relaxations, traced.counters.relaxations);
    ASSERT_EQ(plain.counters.successful_relaxations, traced.counters.successful_relaxations);
    ASSERT_EQ(traced.counters.settled_nodes, rec.settled_seen());
    ASSERT_EQ(traced.counters.successful_relaxations, rec.relaxed_seen());
    ASSERT_EQ(traced.counters.pq_pops, rec.pops_seen());
    ASSERT_EQ(traced.counters.stale_pops, rec.stale_seen());
}

TEST(Trace, ObservingDoesNotChangeTheSearch) {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        const Graph  g = random_connected_graph(seed);
        const NodeId t = static_cast<NodeId>(g.num_nodes() - 1);
        check_observed_equals_unobserved<BinaryHeap<Dist>>(g, 0, t);
        check_observed_equals_unobserved<PairingHeap<Dist>>(g, 0, t);
        check_observed_equals_unobserved<FibonacciHeap<Dist>>(g, 0, t);
        check_observed_equals_unobserved<RadixHeap<Dist>>(g, 0, t);
        check_observed_equals_unobserved<Lpq>(g, 0, t);
        check_observed_equals_unobserved<Lpq>(g, 0, kInvalidNode);
    }
}

std::string traced_dijkstra(const Graph& g, NodeId s, NodeId t, TraceLevel level,
                            std::uint64_t cap, ShortestPathResult<W>& out) {
    std::ostringstream sink;
    TraceRecorder      rec(sink, level, cap, false);
    BinaryHeap<Dist>   q;
    q.reserve_nodes(g.num_nodes());
    DijkstraOptions o;
    o.target = t;
    rec.run_started("dijkstra", "binary_heap", s, t);
    out = dijkstra<W, BinaryHeap<Dist>, TraceRecorder>(g, s, q, o, &rec);
    if (out.reachable(t)) rec.path_reconstructed(out.path_to(t), static_cast<double>(out.dist[t]));
    rec.run_completed(to_string(out.termination), out.reachable(t),
                      static_cast<double>(out.dist[t]));
    return sink.str();
}

TEST(Trace, EventsReplayToTheEngineResult) {
    for (std::uint64_t seed = 100; seed < 130; ++seed) {
        const Graph  g = random_connected_graph(seed);
        const NodeId t = static_cast<NodeId>(g.num_nodes() - 1);
        ShortestPathResult<W> r;
        const auto events = parse(traced_dijkstra(g, 0, t, TraceLevel::kVisual, 1'000'000, r));

        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events.front().type, "RUN_STARTED");
        EXPECT_EQ(events.back().type, "RUN_COMPLETED");
        for (std::size_t i = 0; i < events.size(); ++i) {
            ASSERT_EQ(events[i].seq, i + 1) << "sequence must be gap-free";
        }

        std::map<NodeId, double> replay;
        replay[0]             = 0.0;
        std::uint64_t settled = 0;
        for (const auto& e : events) {
            if (e.type == "EDGE_RELAXED") {
                const auto to = static_cast<NodeId>(e.num.at("to"));
                // A relaxation only ever lowers a tentative distance.
                if (replay.count(to)) {
                    ASSERT_LT(e.num.at("new_distance"), replay[to]);
                }
                replay[to] = e.num.at("new_distance");
            } else if (e.type == "NODE_SETTLED") {
                ++settled;
                const auto n = static_cast<NodeId>(e.num.at("node"));
                ASSERT_EQ(e.num.at("distance"), static_cast<double>(r.dist[n]));
            }
        }
        EXPECT_EQ(settled, r.counters.settled_nodes);
        for (const auto& [node, d] : replay) {
            // Every relaxed node's last relaxation is its engine distance.
            EXPECT_EQ(d, static_cast<double>(r.dist[node])) << "node " << node;
        }
        const bool reached = r.reachable(t);
        bool       saw_path = false;
        for (const auto& e : events) {
            if (e.type == "PATH_RECONSTRUCTED") {
                saw_path = true;
                std::string expect = "\"path\":[";
                const auto  path   = r.path_to(t);
                for (std::size_t i = 0; i < path.size(); ++i) {
                    expect += (i ? "," : "") + std::to_string(path[i]);
                }
                expect += "]";
                EXPECT_NE(e.line.find(expect), std::string::npos);
            }
        }
        EXPECT_EQ(saw_path, reached);
    }
}

TEST(Trace, CappedTraceCountsDropsExactly) {
    const Graph g = lpq_test::grid_graph<W>(20, 20);
    ShortestPathResult<W> r;
    const auto full     = parse(traced_dijkstra(g, 0, 399, TraceLevel::kFull, 10'000'000, r));
    const auto capped_s = traced_dijkstra(g, 0, 399, TraceLevel::kFull, 100, r);
    const auto capped   = parse(capped_s);

    // The always-written closing events come on top of the cap.
    const auto& done = capped.back();
    ASSERT_EQ(done.type, "RUN_COMPLETED");
    EXPECT_LE(capped.size(), 100U + 3U);
    EXPECT_EQ(done.num.at("events_written"), static_cast<double>(capped.size()));
    EXPECT_EQ(done.num.at("events_written") + done.num.at("events_dropped"),
              static_cast<double>(full.size()));
    EXPECT_NE(capped_s.find("\"truncated\":true"), std::string::npos);
}

TEST(Trace, LearnedQueueEventsMatchItsCounters) {
    for (std::uint64_t seed = 200; seed < 230; ++seed) {
        const Graph  g = random_connected_graph(seed);
        const NodeId t = static_cast<NodeId>(g.num_nodes() - 1);

        Lpq plain_q = make<Lpq>(g);
        plain_q.reserve_nodes(g.num_nodes());
        DijkstraOptions o;
        o.target         = t;
        const auto plain = dijkstra<W, Lpq>(g, 0, plain_q, o);

        std::ostringstream sink;
        TraceRecorder      rec(sink, TraceLevel::kVisual, 1'000'000, false);
        Lpq                q = make<Lpq>(g);
        q.reserve_nodes(g.num_nodes());
        rec.attach_lpq(
            [&q](LpqSnapshot& s, std::size_t cap) {
                auto& sub    = q.substrate();
                s.queue_size = sub.size();
                if (sub.size() == 0) return;
                s.bucket_size = sub.current_bucket_size();
                const auto f  = sub.current_front();
                s.bucket_base = static_cast<double>(sub.base());
                s.bound       = static_cast<double>(sub.current_bound());
                for (std::size_t i = 0; i < f.size() && i < cap; ++i) {
                    s.front.push_back({f[i].node, static_cast<double>(f[i].key), f[i].score});
                }
            },
            nullptr);
        q.set_observer(&rec);
        const auto traced = dijkstra<W, Lpq, TraceRecorder>(g, 0, q, o, &rec);

        ASSERT_EQ(plain.dist, traced.dist);
        ASSERT_EQ(plain_q.metrics().extractions, q.metrics().extractions);
        ASSERT_EQ(plain_q.metrics().verified_extractions, q.metrics().verified_extractions);
        ASSERT_EQ(plain_q.metrics().fallback_scans, q.metrics().fallback_scans);

        std::uint64_t accepted = 0;
        std::uint64_t fallback = 0;
        std::uint64_t buckets  = 0;
        for (const auto& e : parse(sink.str())) {
            accepted += e.type == "LPQ_VERIFICATION_ACCEPTED";
            fallback += e.type == "LPQ_FALLBACK_STARTED";
            buckets += e.type == "LPQ_BUCKET_SELECTED";
        }
        // A resolved extraction whose removal then failed would be counted as a
        // fallback by the queue; window_divergences says that never happened.
        ASSERT_EQ(q.metrics().window_divergences, 0U);
        EXPECT_EQ(accepted, q.metrics().verified_extractions);
        EXPECT_EQ(fallback, q.metrics().fallback_scans);
        EXPECT_EQ(buckets, q.metrics().extractions);
    }
}

TEST(Trace, AstarRelaxationsCarryTheHeuristicTerm) {
    const Graph g = lpq_test::grid_graph<W>(12, 12);
    const double scale = euclidean_admissible_scale(g);
    std::ostringstream sink;
    TraceRecorder      rec(sink, TraceLevel::kVisual, 1'000'000, true);
    BinaryHeap<Dist>   q;
    q.reserve_nodes(g.num_nodes());
    EuclideanHeuristic<W> h(scale);
    AstarOptions          o;
    o.target     = 143;
    const auto r = astar<W, BinaryHeap<Dist>, EuclideanHeuristic<W>, TraceRecorder>(g, 0, q, h, o,
                                                                                    &rec);
    ASSERT_TRUE(r.reachable(143));
    std::size_t with_h = 0;
    for (const auto& e : parse(sink.str())) {
        if (e.type == "EDGE_RELAXED") {
            ASSERT_TRUE(e.num.count("h"));
            EXPECT_GE(e.num.at("h"), 0.0);
            EXPECT_EQ(e.num.at("key"), e.num.at("new_distance") + e.num.at("h"));
            ++with_h;
        }
    }
    EXPECT_EQ(with_h, r.counters.successful_relaxations);
}

}  // namespace
