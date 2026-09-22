// =============================================================================
// Candidate manager (ESD.md section 10, ESD.md section 15, PRD.md section 13).
//
// ESD.md section 10: "Maintains a candidate set/window generated from
// predictions." This is that component, and only that component.
//
// -----------------------------------------------------------------------------
// What this class deliberately does NOT do
// -----------------------------------------------------------------------------
//
// It does not reach into a priority queue to find candidates. Candidates are
// OFFERED to it by the LPQ, which is the only part of the system that knows
// which queue is installed.
//
// That boundary is not bureaucratic. Of the five classical queues this project
// implements, only the d-ary heap can cheaply expose more than its minimum: a
// pairing heap's second-smallest element is not locatable without a merge, and
// a Fibonacci heap's root list is unordered. A candidate manager that pulled
// its own candidates would therefore have to know the queue's representation,
// and the "independent modules" requirement in ESD.md section 4.3 would be
// gone -- along with the ability to test candidate policy without a graph, a
// search, or a queue, which is what the tests below actually do.
//
// It also performs no verification. Deciding whether the best-scoring candidate
// may be extracted is the verification engine's job, and keeping the two apart
// is what allows the same candidate policy to be measured under the exact and
// the approximate correctness policies without being reimplemented.
//
// -----------------------------------------------------------------------------
// Window sizing
// -----------------------------------------------------------------------------
//
// ESD.md section 15 gives the starting policy:
//
//     k = max(k_min, ceil(alpha * log2(queue_size + 1)))
//
// and is explicit that this "is a starting policy, not a final research
// claim". It is implemented exactly as written, with a configurable maximum,
// because ESD.md section 15 also requires that "candidate windows must never
// grow without a configurable maximum".
//
// The logarithm is genuinely computed rather than approximated, except in the
// common case alpha == 1, where ceil(log2(n + 1)) equals bit_width(n) exactly
// for n >= 1 and the whole expression becomes a single integer instruction.
// The two paths are asserted equal in the tests, so the fast path is a
// measured equivalence rather than a hopeful one.
//
// This runs once per extraction, not once per candidate, so even the
// floating-point path is cheap next to the O(log n) queue operation it
// accompanies.
//
// -----------------------------------------------------------------------------
// Why an insertion-sorted array rather than a heap
// -----------------------------------------------------------------------------
//
// The window holds at most k_max entries, and k_max is small -- tens, not
// thousands. At that size a sorted array beats a heap on every operation that
// matters here:
//
//   * The common case is REJECTION. Once the window is full, a candidate
//     scoring below the current worst is rejected in one comparison. A heap
//     would need the same comparison and then nothing else, so this is a tie --
//     but the array needs no heap invariant to restore afterwards.
//
//   * Verification consumes the candidates IN ORDER, best first, and may walk
//     several of them. A sorted array is already in that order; a heap would
//     have to be drained, destroying it for the retry path.
//
//   * Expansion re-uses the existing contents. Widening a sorted array is a
//     capacity change and nothing else.
// =============================================================================
#ifndef LPQ_LPQ_CANDIDATE_MANAGER_HPP
#define LPQ_LPQ_CANDIDATE_MANAGER_HPP

#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/types.hpp"
#include "lpq/models/prediction.hpp"

namespace lpq {

/// One candidate under consideration for extraction.
///
/// Carries both the predicted score and the AUTHORITATIVE key. The key is what
/// makes verification possible at all: the whole safety argument rests on being
/// able to check a prediction against the real ordering, so a candidate that
/// travelled without its true key would be unverifiable.
struct Candidate {
    NodeId node = 0;

    /// The candidate's real key in the authoritative queue: g for Dijkstra,
    /// g + h for A*. Never derived from a prediction.
    double key = 0.0;

    /// Predicted ordering signal, higher is better. See models/prediction.hpp.
    float score = 0.0F;

    /// Calibrated probability that this is the correct next extraction.
    float confidence = 0.0F;
};

/// Orders candidates best-predicted-first, with a deterministic tie-break.
///
/// The tie-break on node id matters for the same reason it does in
/// PredictionBetter: without it, two equally scored candidates would be ordered
/// by whichever happened to be offered first, and the search path would depend
/// on enumeration order rather than on the seed.
struct CandidateBetter {
    [[nodiscard]] bool operator()(const Candidate& a, const Candidate& b) const noexcept {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.node < b.node;
    }
};

/// Absolute ceiling on a candidate window, independent of configuration.
///
/// ESD.md section 15 requires a CONFIGURABLE maximum; this is a separate, hard
/// one, and it exists for two reasons.
///
/// The first is arithmetic: the window is allocated once at k_max and reused,
/// so a k_max of a billion would try to reserve tens of gigabytes before the
/// first query ran.
///
/// The second is that such a window would be pointless anyway. The sizing
/// policy grows like log2 of the queue, which reaches 4096 only for a queue of
/// roughly 2^4096 entries. A window that large is no longer a window -- it is a
/// linear scan of the queue, which is the O(n^2) Dijkstra the reference
/// implementation already provides as an oracle.
inline constexpr std::uint32_t kMaxSupportedWindow = 4096;

/// Window-sizing and expansion policy (ESD.md sections 15 and 17).
struct CandidateWindowConfig {
    /// Smallest window. Two, not one: a window of one cannot be wrong in an
    /// interesting way -- there is nothing to compare the top candidate
    /// against, so verification degenerates into "check the single candidate
    /// against the queue", which is just the classical extraction with extra
    /// steps.
    std::uint32_t k_min = 2;

    /// Hard ceiling. ESD.md section 15: windows "must never grow without a
    /// configurable maximum". Also the storage bound: the window is allocated
    /// once at this size and never reallocated during a search.
    std::uint32_t k_max = 32;

    /// Multiplier on log2(queue_size + 1) in the sizing formula.
    double alpha = 1.0;

    /// Window growth on a verification failure, as a multiplier.
    std::uint32_t expansion_factor = 2;

    /// How many times one extraction may widen before the caller should fall
    /// back to the authoritative queue.
    ///
    /// Bounded because the failure mode it guards against is not a wrong
    /// answer but an unbounded one: a search in which every extraction expands
    /// to k_max before succeeding does strictly more work than the classical
    /// queue it was supposed to beat, and would show up as a mysterious
    /// slowdown rather than as an error.
    std::uint32_t max_expansions = 3;

    void validate() const {
        if (k_min == 0) {
            throw InvalidConfigError("k_min must be at least 1");
        }
        if (k_max < k_min) {
            throw InvalidConfigError("k_max (" + std::to_string(k_max) +
                                     ") must be at least k_min (" + std::to_string(k_min) + ")");
        }
        if (k_max > kMaxSupportedWindow) {
            throw InvalidConfigError("k_max (" + std::to_string(k_max) +
                                     ") exceeds the supported window ceiling of " +
                                     std::to_string(kMaxSupportedWindow) +
                                     "; a window that large is a linear scan of the queue, "
                                     "not a candidate window");
        }
        if (!(alpha > 0.0) || !std::isfinite(alpha)) {
            throw InvalidConfigError("alpha must be finite and strictly positive");
        }
        if (expansion_factor < 2) {
            throw InvalidConfigError(
                "expansion_factor must be at least 2; a factor of 1 would retry the same "
                "window forever without making progress");
        }
    }
};

/// Counters required by PRD.md section 13 ("the system must measure ...") and
/// ESD.md section 22.
///
/// Held as plain counters rather than behind the instrumentation switch,
/// because these are not diagnostics: window size and correction counts are
/// primary experimental results, and a build that could not report them could
/// not produce the paper's central table.
struct CandidateMetrics {
    std::uint64_t extractions          = 0;  ///< how many windows were opened
    std::uint64_t candidates_offered   = 0;
    std::uint64_t candidates_admitted  = 0;
    std::uint64_t candidates_rejected  = 0;  ///< offered but scored out of the window
    std::uint64_t expansions           = 0;  ///< windows widened after a failure
    std::uint64_t expansions_exhausted = 0;  ///< widened to the limit and gave up
    /// Sum of the FINAL window size of each extraction -- the size after any
    /// expansions, not the size it opened at. Summing the opening size would
    /// understate exactly the cases that matter, since an extraction only
    /// widens when the prediction was wrong.
    std::uint64_t window_size_sum = 0;
    std::uint32_t max_window_used = 0;

    /// Mean final window size across extractions, or 0 if none.
    [[nodiscard]] double mean_window_size() const noexcept {
        return extractions == 0 ? 0.0
                                : static_cast<double>(window_size_sum) /
                                      static_cast<double>(extractions);
    }

    /// Fraction of offered candidates that did not fit. High values mean the
    /// window is too narrow for the predictor's discrimination.
    [[nodiscard]] double rejection_rate() const noexcept {
        return candidates_offered == 0
                   ? 0.0
                   : static_cast<double>(candidates_rejected) /
                         static_cast<double>(candidates_offered);
    }

    void reset() noexcept { *this = CandidateMetrics{}; }
};

/// Computes ceil(alpha * log2(queue_size + 1)), the ESD.md section 15 policy.
///
/// Exposed separately from the class so it can be tested directly and so the
/// adaptation controller can ask what a window WOULD be without opening one.
[[nodiscard]] inline std::uint32_t window_size_for(std::uint64_t queue_size,
                                                   const CandidateWindowConfig& config) noexcept {
    std::uint64_t k = 0;

    if (queue_size == 0) {
        k = 0;
    } else if (config.alpha == 1.0) {
        // ceil(log2(n + 1)) == bit_width(n) for every n >= 1. Exact, integer,
        // and one instruction. The equivalence is asserted in the tests rather
        // than assumed.
        k = std::bit_width(queue_size);
    } else {
        const double exact = std::ceil(config.alpha *
                                       std::log2(static_cast<double>(queue_size) + 1.0));
        k = exact <= 0.0 ? 0 : static_cast<std::uint64_t>(exact);
    }

    if (k < config.k_min) {
        k = config.k_min;
    }
    if (k > config.k_max) {
        k = config.k_max;
    }
    return static_cast<std::uint32_t>(k);
}

/// Holds the bounded, score-ordered candidate window for one extraction.
class CandidateManager {
public:
    CandidateManager() { storage_.reserve(config_.k_max); }

    explicit CandidateManager(CandidateWindowConfig config) : config_(config) {
        config_.validate();
        // Allocated once, at the ceiling, and never grown again. An allocation
        // inside the extraction loop would be both a latency spike and a
        // source of run-to-run variance in exactly the measurement this
        // project exists to make.
        storage_.reserve(config_.k_max);
    }

    [[nodiscard]] const CandidateWindowConfig& config() const noexcept { return config_; }

    // --- Lifecycle -----------------------------------------------------------

    /// Opens a window for one extraction from a queue of the given size.
    ///
    /// O(1): the storage is reused, not reallocated.
    void begin_extraction(std::uint64_t queue_size) noexcept {
        storage_.clear();
        target_    = window_size_for(queue_size, config_);
        expansions_ = 0;

        ++metrics_.extractions;
        metrics_.window_size_sum += target_;
        metrics_.max_window_used = std::max(metrics_.max_window_used, target_);
    }

    /// Offers a candidate. Returns true if it was admitted to the window.
    ///
    /// Rejection is the common case once the window is full, and it costs one
    /// comparison: a candidate no better than the current worst cannot enter.
    ///
    /// CONTRACT: a node must not be offered twice within one extraction.
    ///
    /// This matters more than it looks. expand() deliberately keeps the
    /// candidates already admitted, so a refill after an expansion must supply
    /// only NEW candidates -- a substrate that restarted its enumeration would
    /// re-offer the same high-scoring entries, they would be admitted a second
    /// time, and the widened window would fill with duplicates instead of
    /// reaching further into the queue. Expansion would then appear to happen
    /// while making no progress at all, and the extraction would fall back for
    /// no visible reason. A real substrate honours this naturally, because its
    /// enumeration continues from where it stopped.
    ///
    /// The check is an assertion rather than a branch: it is O(k) per offer,
    /// which is real cost on the hottest path in the system, and a violation is
    /// a caller bug rather than a runtime condition. It is active in the Debug
    /// and sanitizer configurations, which run the whole test suite, and
    /// compiles out entirely under NDEBUG.
    bool offer(const Candidate& c) noexcept {
        assert(rank_of(c.node) == storage_.size() &&
               "a node was offered twice in one extraction; see the contract on offer()");

        ++metrics_.candidates_offered;

        const CandidateBetter better;

        if (storage_.size() >= target_) {
            // Full. Only a candidate better than the current worst can enter.
            if (!better(c, storage_.back())) {
                ++metrics_.candidates_rejected;
                return false;
            }
            storage_.pop_back();
        }

        // Insertion sort into the already-ordered array. At most k_max
        // comparisons, and typically far fewer, because a candidate that only
        // just beat the worst entry stops near the end.
        const auto at = std::upper_bound(storage_.begin(), storage_.end(), c, better);
        storage_.insert(at, c);

        ++metrics_.candidates_admitted;
        return true;
    }

    /// Widens the window after a verification failure (ESD.md section 16).
    ///
    /// Returns false when the window is already at its maximum or the
    /// expansion budget is spent, which is the caller's signal to fall back to
    /// the authoritative queue. Existing candidates are kept: they were scored
    /// against the same predictions and re-offering them would be wasted work.
    [[nodiscard]] bool expand() noexcept {
        if (expansions_ >= config_.max_expansions || target_ >= config_.k_max) {
            ++metrics_.expansions_exhausted;
            return false;
        }

        const std::uint32_t previous = target_;
        const std::uint64_t widened =
            static_cast<std::uint64_t>(target_) * config_.expansion_factor;
        target_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(widened, config_.k_max));

        // Only the increment, so the running sum tracks each extraction's
        // final window rather than counting the opening size twice.
        metrics_.window_size_sum += (target_ - previous);

        ++expansions_;
        ++metrics_.expansions;
        metrics_.max_window_used = std::max(metrics_.max_window_used, target_);
        return true;
    }

    // --- Access --------------------------------------------------------------

    /// The window, ordered best-predicted-first.
    [[nodiscard]] std::span<const Candidate> candidates() const noexcept {
        return {storage_.data(), storage_.size()};
    }

    /// The best-scoring candidate. Precondition: the window is not empty.
    [[nodiscard]] const Candidate& best() const noexcept { return storage_.front(); }

    /// The candidate holding the smallest AUTHORITATIVE key in the window.
    ///
    /// Distinct from best(), and the distinction is the whole point of
    /// verification: best() is what the model believes, this is what the keys
    /// say. When they disagree, the prediction was wrong, and the verification
    /// engine needs both to say so and to record by how much.
    ///
    /// Ties break on node id so the result does not depend on offer order.
    [[nodiscard]] const Candidate& min_key_candidate() const noexcept {
        const Candidate* found = &storage_.front();
        for (const Candidate& c : storage_) {
            if (c.key < found->key || (c.key == found->key && c.node < found->node)) {
                found = &c;
            }
        }
        return *found;
    }

    /// Position of a node in the window, or storage size if absent.
    [[nodiscard]] std::size_t rank_of(NodeId u) const noexcept {
        for (std::size_t i = 0; i < storage_.size(); ++i) {
            if (storage_[i].node == u) {
                return i;
            }
        }
        return storage_.size();
    }

    [[nodiscard]] bool          empty() const noexcept { return storage_.empty(); }
    [[nodiscard]] std::size_t   size() const noexcept { return storage_.size(); }
    [[nodiscard]] bool          full() const noexcept { return storage_.size() >= target_; }
    [[nodiscard]] std::uint32_t target_window() const noexcept { return target_; }
    [[nodiscard]] std::uint32_t expansions_this_extraction() const noexcept {
        return expansions_;
    }

    /// Whether another expansion is available without attempting one.
    [[nodiscard]] bool can_expand() const noexcept {
        return expansions_ < config_.max_expansions && target_ < config_.k_max;
    }

    // --- Metrics -------------------------------------------------------------

    [[nodiscard]] const CandidateMetrics& metrics() const noexcept { return metrics_; }
    void                                  reset_metrics() noexcept { metrics_.reset(); }

    /// Bytes of candidate storage. Reported in the memory column of the
    /// results schema, where LPQ's overhead against a classical queue has to be
    /// accounted for honestly.
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return storage_.capacity() * sizeof(Candidate);
    }

private:
    CandidateWindowConfig  config_{};
    std::vector<Candidate> storage_;   ///< ordered best-first, size <= target_
    std::uint32_t          target_     = 0;
    std::uint32_t          expansions_ = 0;
    CandidateMetrics       metrics_{};
};

}  // namespace lpq

#endif  // LPQ_LPQ_CANDIDATE_MANAGER_HPP
