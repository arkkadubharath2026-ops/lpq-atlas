// =============================================================================
// Per-search dynamic state feeding the feature extractor (PRD.md section 9).
//
// PRD.md section 9 asks for per-node counts of successful and failed
// relaxations, queue insertions, decrease-key operations, hop depth and time
// since discovery, plus aggregate search-state signals. None of these exist in
// a plain Dijkstra; they have to be recorded as the search runs.
//
// -----------------------------------------------------------------------------
// The cost of observing the search
// -----------------------------------------------------------------------------
//
// This bookkeeping is NOT free, and pretending otherwise would corrupt the
// central research question. Every counter update sits in the relaxation loop,
// which is the hottest loop in the program. If the classical baselines were
// measured without it and LPQ with it, LPQ would carry a handicap that has
// nothing to do with learning; if the baselines were measured WITH it, they
// would carry a cost they do not need.
//
// The resolution is that this state is a separate object that Dijkstra does not
// know about. Classical runs never construct it and pay nothing. LPQ runs pay
// for it, and that cost is part of LPQ's measured overhead -- which is the
// honest accounting PRD.md section 41 demands ("end-to-end wall-clock
// evaluation including feature extraction").
//
// -----------------------------------------------------------------------------
// Reset cost
// -----------------------------------------------------------------------------
//
// The per-node arrays are sized once per graph and reused across queries. A
// naive reset would memset several arrays of n entries between queries, which
// on a large graph with short target-mode queries would dominate the query
// itself. Instead each node carries a generation stamp: a node whose stamp is
// stale is treated as fresh, so a reset is a single integer increment.
// =============================================================================
#ifndef LPQ_FEATURES_SEARCH_STATE_HPP
#define LPQ_FEATURES_SEARCH_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <vector>

#include "lpq/common/types.hpp"

namespace lpq {

/// Dynamic per-node counters, packed into exactly 16 bytes so four share a
/// cache line.
///
/// The relaxation counts are 16-bit and saturating. A node's relaxation count
/// is bounded in practice by its in-degree, which on the graph families this
/// project targets is far below 65535; a node that did exceed it would report
/// the clamped maximum, which orders correctly against every smaller count.
/// Widening these to 32 bits pushes the struct to 20 bytes and drops the
/// per-cache-line density from four nodes to three, which costs more in the
/// relaxation loop than the extra range is worth.
struct NodeDynamicState {
    std::uint16_t successful_relaxations = 0;
    std::uint16_t failed_relaxations     = 0;
    std::uint16_t insertions             = 0;
    std::uint16_t decrease_keys          = 0;
    /// Hops from the source along the current best path.
    std::uint16_t hop_depth              = 0;
    std::uint16_t reserved_              = 0;
    /// Value of the settled counter when this node was first discovered.
    /// Queue age is derived as (settled_now - discovered_at).
    std::uint32_t discovered_at_settled  = 0;
};

static_assert(sizeof(NodeDynamicState) == 16,
              "NodeDynamicState is expected to stay compact; it is touched in the "
              "relaxation loop");

/// Aggregate signals describing the search as a whole (ESD.md section 11).
struct SearchState {
    /// Key of the current queue minimum.
    double current_best_key = 0.0;
    std::uint64_t queue_size     = 0;
    std::uint64_t settled_nodes  = 0;
    std::uint64_t relaxations    = 0;

    /// Exponentially weighted recent prediction error and correction rate.
    /// Zero until the predictor has produced feedback, which is the correct
    /// starting value: no evidence of error yet.
    double recent_prediction_error = 0.0;
    double recent_correction_rate  = 0.0;

    /// Set for A*; zero for Dijkstra. Recorded per candidate by the extractor.
    double distance_to_target = 0.0;
};

/// Per-node dynamic state for one search, reusable across queries.
class SearchObservations {
public:
    SearchObservations() = default;

    explicit SearchObservations(std::size_t node_count) { reserve_nodes(node_count); }

    /// Sizes the arrays for a graph. Called once per graph.
    void reserve_nodes(std::size_t node_count) {
        state_.assign(node_count, NodeDynamicState{});
        generation_.assign(node_count, 0U);
        current_generation_ = 1U;
    }

    /// Begins a new query. O(1).
    ///
    /// Bumping the generation invalidates every node's state without touching
    /// it. The wrap case is handled explicitly: on the (2^32 - 1)-th query the
    /// stamps are genuinely cleared, because otherwise a stale stamp could
    /// alias the new generation and a node would appear to carry counters from
    /// a query run four billion queries earlier.
    void begin_query() noexcept {
        if (current_generation_ == std::numeric_limits<std::uint32_t>::max()) {
            std::fill(generation_.begin(), generation_.end(), 0U);
            current_generation_ = 1U;
        } else {
            ++current_generation_;
        }
    }

    /// The node's state, freshly zeroed if it has not been touched this query.
    [[nodiscard]] NodeDynamicState& node(NodeId u) noexcept {
        if (generation_[u] != current_generation_) {
            state_[u]      = NodeDynamicState{};
            generation_[u] = current_generation_;
        }
        return state_[u];
    }

    /// Read-only access that does not stamp the node.
    ///
    /// Returns a zeroed reference for untouched nodes, so a candidate never
    /// reads another query's counters.
    [[nodiscard]] const NodeDynamicState& node_or_zero(NodeId u) const noexcept {
        static const NodeDynamicState kZero{};
        return generation_[u] == current_generation_ ? state_[u] : kZero;
    }

    [[nodiscard]] bool touched(NodeId u) const noexcept {
        return generation_[u] == current_generation_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return state_.size(); }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return state_.capacity() * sizeof(NodeDynamicState) +
               generation_.capacity() * sizeof(std::uint32_t);
    }

    // --- Recording events ----------------------------------------------------
    //
    // Called from the relaxation loop when LPQ is active. Counters saturate
    // rather than wrap: the 16-bit fields would otherwise roll over on a
    // pathological node and present a large count as a small one, which is a
    // worse error than a clamped maximum.

    void on_discovered(NodeId u, std::uint64_t settled_now, std::uint16_t hop_depth) noexcept {
        NodeDynamicState& s = node(u);
        s.discovered_at_settled = static_cast<std::uint32_t>(
            settled_now > std::numeric_limits<std::uint32_t>::max()
                ? std::numeric_limits<std::uint32_t>::max()
                : settled_now);
        s.hop_depth = hop_depth;
    }

    void on_successful_relaxation(NodeId u, std::uint16_t hop_depth) noexcept {
        NodeDynamicState& s = node(u);
        if (s.successful_relaxations != std::numeric_limits<std::uint16_t>::max()) {
            ++s.successful_relaxations;
        }
        s.hop_depth = hop_depth;
    }

    void on_failed_relaxation(NodeId u) noexcept {
        NodeDynamicState& s = node(u);
        if (s.failed_relaxations != std::numeric_limits<std::uint16_t>::max()) {
            ++s.failed_relaxations;
        }
    }

    void on_insertion(NodeId u) noexcept {
        NodeDynamicState& s = node(u);
        if (s.insertions != std::numeric_limits<std::uint16_t>::max()) {
            ++s.insertions;
        }
    }

    void on_decrease_key(NodeId u) noexcept {
        NodeDynamicState& s = node(u);
        if (s.decrease_keys != std::numeric_limits<std::uint16_t>::max()) {
            ++s.decrease_keys;
        }
    }

private:
    std::vector<NodeDynamicState> state_;
    std::vector<std::uint32_t>    generation_;
    std::uint32_t                 current_generation_ = 1U;
};

}  // namespace lpq

#endif  // LPQ_FEATURES_SEARCH_STATE_HPP
