// =============================================================================
// Permanent regression seed corpus.
//
// PROJECT RULE: every seed that has ever produced a failure is recorded here
// permanently, with the date, the symptom and the cause. A seed that found a
// bug once is evidence that the bug's shape is reachable, so it is replayed on
// every future run rather than left to the chance of a random sweep
// rediscovering it.
//
// This corpus therefore only ever grows. Entries are never removed, even after
// the underlying bug is fixed -- the fix is exactly what the entry now guards.
//
// The randomised sweeps elsewhere in the suite use a moving range of seeds and
// serve a different purpose: they look for *new* failures. This list is the
// memory of old ones. Both run, and a seed discovered by a sweep is promoted
// here rather than left in the sweep's range, because sweep ranges get tuned
// and a bug-finding seed must not depend on that tuning.
//
// HOW TO ADD AN ENTRY
//
//   1. Note the seed printed in the failure message.
//   2. Add it below with the date, what broke, and the root cause.
//   3. Confirm it reproduces before the fix and passes after.
//
// The corpus is currently empty: no randomised sweep in this project has
// produced a failure that survived to be recorded. The mechanism is in place so
// that the first one is captured rather than lost. It is deliberately NOT
// pre-populated with invented entries -- a regression test that never
// corresponded to a real defect documents nothing.
// =============================================================================
#ifndef LPQ_TEST_REGRESSION_SEEDS_HPP
#define LPQ_TEST_REGRESSION_SEEDS_HPP

#include <cstdint>
#include <string_view>
#include <vector>

namespace lpq_test {

/// A seed that once exposed a defect, retained permanently.
struct RegressionSeed {
    std::uint64_t    seed;
    /// ISO date the failure was first observed.
    std::string_view date;
    /// What went wrong, and why. Written for someone reading it years later
    /// with no memory of the incident.
    std::string_view description;
};

/// Seeds that exposed a defect in graph construction or metadata.
inline const std::vector<RegressionSeed>& graph_regression_seeds() {
    static const std::vector<RegressionSeed> kSeeds{
        // (empty -- see the header comment)
    };
    return kSeeds;
}

/// Seeds that exposed a defect in a priority queue.
inline const std::vector<RegressionSeed>& queue_regression_seeds() {
    static const std::vector<RegressionSeed> kSeeds{
        // (empty -- see the header comment)
    };
    return kSeeds;
}

/// Seeds that exposed a disagreement between shortest-path implementations.
inline const std::vector<RegressionSeed>& differential_regression_seeds() {
    static const std::vector<RegressionSeed> kSeeds{
        {1002, "2026-09-22",
         "Single-target differential comparison reported false failures. The "
         "O(n^2) reference had discovered nodes 6, 10, 13 and 20 before the "
         "target settled; the binary heap had not touched them, and the 4-ary "
         "heap had touched all but node 10. Every implementation was correct: "
         "in target mode a result's reachable() means 'discovered before the "
         "search stopped', not 'reachable in the graph', and queues reach the "
         "target having explored different frontiers. The defect was in the "
         "comparison policy, which compared reachability unconditionally. Fixed "
         "by restricting reachability comparison to full-SSSP mode and "
         "comparing, in target mode, only nodes both runs settled plus the "
         "target and the termination reason. Kept because the same mistake is "
         "easy to reintroduce when LPQ adds its own early-exit conditions."},
    };
    return kSeeds;
}

}  // namespace lpq_test

#endif  // LPQ_TEST_REGRESSION_SEEDS_HPP
