// =============================================================================
// Differential comparison policy (ESD.md section 24).
//
// "Run the same query through reference Dijkstra, Binary Heap, Pairing Heap,
//  Radix Heap where valid, Fibonacci Heap, LPQ. Compare outputs automatically."
//
// Deciding whether two results "agree" is a policy question, not a data
// property, so it lives here rather than on ShortestPathResult. Three rules,
// each chosen because the naive alternative produces false failures or, worse,
// false passes:
//
//  1. COSTS are compared exactly for integer weights and with a relative
//     tolerance for doubles. Two correct implementations settle nodes in
//     different orders and therefore sum the same path in a different
//     association order; demanding bit equality on doubles would fail correct
//     code. See numeric.hpp for why the tolerance is 1e-9.
//
//  2. PREDECESSOR ARRAYS are never compared element-wise. When several shortest
//     paths share the optimal cost, which one is recorded depends on the
//     queue's tie-breaking, and every choice is equally correct. Instead the
//     path is reconstructed and its cost recomputed from the graph: that
//     verifies the parent array is *a* valid optimal path without demanding it
//     be a *particular* one.
//
//  3. REACHABILITY is compared exactly in full single-source mode, where it is
//     a property of the graph and no implementation may disagree about it.
//
//     In single-target mode it is NOT comparable, because there `reachable(v)`
//     means "discovered before the search stopped", not "reachable in the
//     graph". A target-mode search halts the moment the target settles, and
//     different queues have touched different frontiers by that point. Seed
//     1002 (see regression_seeds.hpp) produced exactly this: the O(n^2)
//     reference had discovered nodes 6, 10, 13 and 20 before reaching the
//     target while the binary heap had not, and all implementations were
//     correct. Target mode therefore compares only nodes both runs settled,
//     plus the target itself and the termination reason -- which is the whole
//     of what a target-mode query actually claims.
//
// Operation counts are deliberately NOT compared: queues legitimately differ in
// pushes, pops and comparisons, and that difference is the very thing the
// benchmarks measure.
// =============================================================================
#ifndef LPQ_TEST_DIFFERENTIAL_HPP
#define LPQ_TEST_DIFFERENTIAL_HPP

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/algorithms/shortest_path_result.hpp"
#include "lpq/common/numeric.hpp"
#include "lpq/common/types.hpp"
#include "lpq/graph/csr_graph.hpp"

namespace lpq_test {

using lpq::costs_equal;
using lpq::CsrGraph;
using lpq::kInvalidNode;
using lpq::NodeId;
using lpq::ShortestPathResult;

/// One difference found between two results.
struct Difference {
    NodeId      node = kInvalidNode;
    std::string what;
};

/// Compares `candidate` against `expected` under the policy above.
///
/// Returns every difference found rather than stopping at the first, so a
/// failure report shows whether one node diverged or the whole array did --
/// which usually distinguishes a tie-breaking subtlety from a broken queue.
template <lpq::WeightType W>
[[nodiscard]] std::vector<Difference> compare_results(const CsrGraph<W>& g,
                                                      const ShortestPathResult<W>& expected,
                                                      const ShortestPathResult<W>& candidate,
                                                      std::size_t max_differences = 16) {
    std::vector<Difference> diffs;

    if (expected.num_nodes() != candidate.num_nodes()) {
        diffs.push_back({kInvalidNode, "node counts differ: " +
                                           std::to_string(expected.num_nodes()) + " vs " +
                                           std::to_string(candidate.num_nodes())});
        return diffs;
    }

    const bool full_sssp = expected.target == kInvalidNode;

    // Target mode makes exactly two claims, and both must hold exactly.
    if (!full_sssp) {
        if (expected.termination != candidate.termination) {
            diffs.push_back({kInvalidNode,
                             std::string("termination differs: expected ") +
                                 lpq::to_string(expected.termination) + ", got " +
                                 lpq::to_string(candidate.termination)});
        }
        const NodeId t = expected.target;
        if (t < expected.num_nodes()) {
            if (expected.reachable(t) != candidate.reachable(t)) {
                diffs.push_back({t, "target reachability differs"});
            } else if (expected.reachable(t) &&
                       !costs_equal<W>(expected.dist[t], candidate.dist[t])) {
                std::ostringstream os;
                os << "target distance differs: expected " << expected.dist[t] << ", got "
                   << candidate.dist[t];
                diffs.push_back({t, os.str()});
            }
        }
    }

    for (NodeId v = 0; v < expected.num_nodes() && diffs.size() < max_differences; ++v) {
        // Only settled nodes carry a final distance. In full SSSP every
        // reachable node is settled, so this is no restriction there; in target
        // mode it is what keeps differing exploration frontiers from being
        // reported as disagreements.
        const bool both_settled = expected.settled[v] && candidate.settled[v];

        if (full_sssp) {
            // Reachability is a property of the graph here, so any disagreement
            // is a genuine bug.
            if (expected.reachable(v) != candidate.reachable(v)) {
                std::ostringstream os;
                os << "reachability differs: expected "
                   << (expected.reachable(v) ? "reachable" : "unreachable") << ", got "
                   << (candidate.reachable(v) ? "reachable" : "unreachable");
                diffs.push_back({v, os.str()});
                continue;
            }
        } else if (!both_settled) {
            continue;  // Frontier difference, not a disagreement.
        }

        if (!expected.reachable(v) || !candidate.reachable(v)) {
            continue;
        }

        if (!costs_equal<W>(expected.dist[v], candidate.dist[v])) {
            std::ostringstream os;
            os << "distance differs: expected " << expected.dist[v] << ", got "
               << candidate.dist[v];
            diffs.push_back({v, os.str()});
            continue;
        }

        // Rule 2: validate the recorded path rather than the parent array.
        const auto path = candidate.path_to(v);
        if (path.empty()) {
            diffs.push_back({v, "reachable but no path could be reconstructed"});
            continue;
        }
        if (path.front() != candidate.source || path.back() != v) {
            diffs.push_back({v, "reconstructed path has wrong endpoints"});
            continue;
        }
        const auto path_cost = candidate.verify_path_cost(g, path);
        if (!costs_equal<W>(path_cost, candidate.dist[v])) {
            std::ostringstream os;
            os << "recorded path costs " << path_cost << " but distance says "
               << candidate.dist[v];
            diffs.push_back({v, os.str()});
        }
    }
    return diffs;
}

/// Renders differences for a test failure message.
inline std::string describe(const std::vector<Difference>& diffs) {
    std::ostringstream os;
    os << diffs.size() << " difference(s):";
    for (const auto& d : diffs) {
        os << "\n  node ";
        if (d.node == kInvalidNode) {
            os << "(n/a)";
        } else {
            os << d.node;
        }
        os << ": " << d.what;
    }
    return os.str();
}

/// Asserts that `candidate` agrees with `expected`, with `context` in the
/// failure message so a failing case is replayable.
template <lpq::WeightType W>
void expect_results_agree(const CsrGraph<W>& g, const ShortestPathResult<W>& expected,
                          const ShortestPathResult<W>& candidate, const std::string& context) {
    const auto diffs = compare_results(g, expected, candidate);
    EXPECT_TRUE(diffs.empty()) << context << "\n" << describe(diffs);
}

/// Verifies the defining optimality condition directly: after a complete
/// search, no arc may still be relaxable.
///
/// Independent of any reference implementation -- it checks the result against
/// the graph itself, so it would catch a bug that happened to affect the
/// reference and the candidate identically.
template <lpq::WeightType W>
void expect_no_relaxable_arc(const CsrGraph<W>& g, const ShortestPathResult<W>& r,
                             const std::string& context) {
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        if (!r.reachable(u) || !r.settled[u]) {
            continue;
        }
        for (const auto& a : g.out_arcs(u)) {
            if (!r.settled[a.target]) {
                continue;
            }
            ASSERT_TRUE(r.reachable(a.target))
                << context << ": node " << a.target << " settled but unreachable";
            const auto via = lpq::saturating_add<W>(r.dist[u], a.weight);
            EXPECT_TRUE(r.dist[a.target] <= via || costs_equal<W>(r.dist[a.target], via))
                << context << ": arc " << u << "->" << a.target
                << " remains relaxable (" << r.dist[a.target] << " > " << via << ")";
        }
    }
}

}  // namespace lpq_test

#endif  // LPQ_TEST_DIFFERENTIAL_HPP
