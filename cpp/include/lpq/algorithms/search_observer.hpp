// =============================================================================
// The seam through which a search can be watched (LPQ Atlas spec sections 8-9).
//
// Dijkstra and A* take an optional observer as a template parameter. The
// default, NullSearchObserver, is not merely empty: every call site is guarded
// by `if constexpr (kObserved<Observer>)`, so with the default the calls are
// absent from the generated code and the searches the benchmark times are the
// same instructions they were before this seam existed. That is the property
// that lets the visualiser and the benchmark share one algorithm
// implementation without the visualiser taxing the measurement.
//
// An observer sees exactly the operations the search performs, in the order it
// performs them. It is never asked to decide anything, so it cannot change
// what the search computes -- a test asserts that an observed run returns the
// same distances, parents and counters as an unobserved one.
// =============================================================================
#ifndef LPQ_ALGORITHMS_SEARCH_OBSERVER_HPP
#define LPQ_ALGORITHMS_SEARCH_OBSERVER_HPP

#include <cstddef>
#include <type_traits>

#include "lpq/common/types.hpp"

namespace lpq {

/// The observer that observes nothing. Its calls are compiled out entirely.
struct NullSearchObserver {};

/// True for every observer except the null one.
template <typename Observer>
inline constexpr bool kObserved = !std::is_same_v<Observer, NullSearchObserver>;

/// How an improved distance reached the queue.
enum class QueueUpdate : unsigned char {
    kPush,         ///< lazy deletion: a new entry was pushed
    kDecreaseKey,  ///< the existing entry's key was lowered (or inserted)
};

// The hooks an observer implements. Documented here rather than enforced with a
// concept so that an observer only has to be written against this list:
//
//   on_source_pushed(NodeId source, double key)
//   on_pop(NodeId node, double key, std::size_t queue_size_after)
//   on_stale_pop(NodeId node, double key)
//   on_settle(NodeId node, double distance)
//   on_edge_examined(NodeId from, NodeId to, double weight, double candidate)
//   on_edge_relaxed(NodeId from, NodeId to, double old_distance,
//                   double new_distance, double queue_key, QueueUpdate how)
//   on_target_reached(NodeId node, double distance)
//
// Distances are passed as double because the observer is a reporting layer:
// it never feeds a value back into the search, so the conversion cannot affect
// correctness, and it keeps observers independent of the weight type.

}  // namespace lpq

#endif  // LPQ_ALGORITHMS_SEARCH_OBSERVER_HPP
