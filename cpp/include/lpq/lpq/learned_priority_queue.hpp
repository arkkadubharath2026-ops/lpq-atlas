// =============================================================================
// The learned priority queue (ESD.md section 10, ESD.md section 39 item 19,
// PRD.md section 3).
//
// Assembles the pieces built in the preceding milestones into something
// Dijkstra can use: the authoritative bucket substrate, a scorer, the candidate
// manager, the verification engine and the correction loop.
//
// It satisfies PriorityQueueLike, so the existing templated Dijkstra and the
// existing differential harness accept it unchanged. That is deliberate: the
// strongest available evidence that the learned queue is correct is that it
// passes, without modification, the same differential sweep that validated the
// five classical queues against the trusted reference.
//
// -----------------------------------------------------------------------------
// Where the work is saved, and what it costs
// -----------------------------------------------------------------------------
//
// A classical bucket queue extracts by scanning the current bucket for the
// minimum: O(bucket). This queue instead:
//
//   1. reads the bucket's best-scoring FRONT, which the substrate maintained at
//      push time, so obtaining it costs nothing at pop time;
//   2. verifies the front against the bucket's cached lower bound, O(k);
//   3. on success, removes the accepted entry and returns -- the rest of the
//      bucket was never touched;
//   4. on failure, falls back to the classical scan.
//
// The cost model is therefore explicit and bounded:
//
//   hit:  O(k) instead of O(bucket)
//   miss: O(k) + O(bucket), i.e. the classical cost plus a bounded overhead
//
// So the queue can lose, but only by the O(k) overhead, and it wins whenever
// the front contains an entry whose key equals the bucket's bound. Whether that
// happens often enough to pay for the overhead is exactly the empirical
// question this project exists to answer, and nothing here presumes the answer.
//
// -----------------------------------------------------------------------------
// Why the score is computed at push time
// -----------------------------------------------------------------------------
//
// Selecting a candidate window by scoring candidates at pop time would require
// examining them, which is the scan being avoided -- the model would have to
// read every entry in order to tell us which entries to read. Scoring at push
// time breaks that circularity: each entry is scored once, when it is inserted,
// and the substrate keeps the best-scoring ones at the front for free.
//
// This is why the Scorer concept demands a cheap per-node score rather than a
// full feature extraction plus inference. StaticNodeScorer satisfies it by
// precomputing every node's score once at graph load, outside any timed region,
// reducing the per-push cost to an array read.
// =============================================================================
#ifndef LPQ_LPQ_LEARNED_PRIORITY_QUEUE_HPP
#define LPQ_LPQ_LEARNED_PRIORITY_QUEUE_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/lpq/bucket_substrate.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/models/predictor.hpp"
#include "lpq/verification/correction_engine.hpp"
#include "lpq/verification/verification_engine.hpp"

namespace lpq {

/// What the LPQ requires of a scorer.
///
/// One cheap number per node. Not a feature vector and not an inference: see
/// the header on why scoring happens at push time and must therefore be far
/// cheaper than a key comparison is expensive.
template <typename S>
concept NodeScorer = requires(const S s, NodeId u) {
    { s.score_of(u) } -> std::same_as<float>;
};

/// Scores every node once, from static graph features, at graph load.
///
/// The per-node feature vector is filled with the STATIC group only. The
/// dynamic, queue and search-state groups are left at zero, and that is not an
/// approximation of them -- it is a statement that this scorer does not use
/// them, which is the price of the score being a constant per node.
///
/// The precomputation is O(n) inferences and happens outside every timed
/// region. What remains on the hot path is one array read per push.
template <PredictorLike Model>
class StaticNodeScorer {
public:
    StaticNodeScorer() = default;

    void build(const GraphFeatures& features, const Model& model) {
        scores_.assign(features.size(), 0.0F);

        FeatureVector x{};
        for (std::size_t u = 0; u < features.size(); ++u) {
            const NodeStaticFeatures& sf = features.node(static_cast<NodeId>(u));

            x.fill(0.0F);
            set_feature(x, Feature::kOutDegree, sf.out_degree);
            set_feature(x, Feature::kMinAdjacentWeight, sf.min_adjacent_weight);
            set_feature(x, Feature::kMeanAdjacentWeight, sf.mean_adjacent_weight);
            set_feature(x, Feature::kMaxAdjacentWeight, sf.max_adjacent_weight);
            set_feature(x, Feature::kAdjacentWeightSpread, sf.weight_spread);
            set_feature(x, Feature::kDegreeRelativeToMean, sf.degree_relative_to_mean);

            scores_[u] = model.predict(x).score;
        }
    }

    [[nodiscard]] float score_of(NodeId u) const noexcept {
        return u < scores_.size() ? scores_[u] : 0.0F;
    }

    [[nodiscard]] std::size_t size() const noexcept { return scores_.size(); }

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return scores_.capacity() * sizeof(float);
    }

private:
    std::vector<float> scores_;
};

/// Scores every node identically. The control condition for the ablation.
///
/// With no ordering information the front is whatever arrived first, so this
/// isolates the cost of the LPQ machinery from the value of its predictions --
/// any speed difference against the classical bucket queue under this scorer is
/// pure overhead.
class ConstantNodeScorer {
public:
    [[nodiscard]] float score_of(NodeId /*u*/) const noexcept { return 0.0F; }
};

/// Everything the LPQ records about its own behaviour.
struct LpqMetrics {
    std::uint64_t extractions          = 0;
    std::uint64_t verified_extractions = 0;  ///< resolved without a full scan
    std::uint64_t fallback_scans       = 0;
    std::uint64_t entries_examined     = 0;  ///< front entries looked at
    std::uint64_t entries_skipped      = 0;  ///< bucket entries never touched

    /// Verification accepted a node the substrate could not then find in the
    /// current bucket. Always zero in a correct run; a non-zero value means the
    /// window and the bucket have diverged. Counted rather than asserted so a
    /// long experiment surfaces it instead of dying, and the extraction still
    /// falls through to the classical scan, which is correct regardless.
    std::uint64_t window_divergences = 0;

    /// Fraction of extractions that avoided the classical scan. The headline
    /// number: this is what the learned component has to make large.
    [[nodiscard]] double verified_rate() const noexcept {
        return extractions == 0 ? 0.0
                                : static_cast<double>(verified_extractions) /
                                      static_cast<double>(extractions);
    }

    /// Entries the queue did not have to touch, per extraction. The actual
    /// saving, in the unit the saving occurs in.
    [[nodiscard]] double mean_entries_skipped() const noexcept {
        return extractions == 0 ? 0.0
                                : static_cast<double>(entries_skipped) /
                                      static_cast<double>(extractions);
    }

    void reset() noexcept { *this = LpqMetrics{}; }
};

/// Dijkstra-compatible priority queue with a learned candidate window.
template <WeightType W, NodeScorer Scorer>
    requires(bucket_substrate_supports<W>())
class LearnedPriorityQueue {
public:
    using KeyType   = DistanceType<W>;
    using Substrate = BucketSubstrate<W>;

    /// Lazy deletion, inherited from the substrate.
    static constexpr bool kSupportsDecreaseKey = false;

    LearnedPriorityQueue() : LearnedPriorityQueue(1, CandidateWindowConfig{}) {}

    LearnedPriorityQueue(KeyType bucket_width, CandidateWindowConfig window,
                         CorrectnessPolicy policy = CorrectnessPolicy::exact())
        : substrate_(bucket_width), manager_(window), verifier_(policy) {
        substrate_.set_front_size(window.k_max);
    }

    void set_scorer(const Scorer* scorer) noexcept { scorer_ = scorer; }

    // --- PriorityQueueLike ---------------------------------------------------

    void push(NodeId node, KeyType key) {
        substrate_.push(node, key, scorer_ == nullptr ? 0.0F : scorer_->score_of(node));
    }

    [[nodiscard]] QueueEntry<KeyType> pop() {
        ++metrics_.extractions;

        const std::size_t bucket_size = substrate_.current_bucket_size();
        const auto        front       = substrate_.current_front();

        if (front.empty()) {
            ++metrics_.fallback_scans;
            return substrate_.pop();
        }

        // --- Build the candidate window from the front, which cost nothing ---
        manager_.begin_extraction(substrate_.size());
        std::size_t offered = 0;
        for (const auto& e : front) {
            if (offered >= manager_.target_window()) {
                break;
            }
            Candidate c;
            c.node  = e.node;
            c.key   = static_cast<double>(e.key);
            c.score = e.score;
            (void)manager_.offer(c);
            ++offered;
        }
        metrics_.entries_examined += offered;

        // --- Verify, expanding into the rest of the bucket if needed ---------
        std::size_t      cursor = offered;
        const QueueBound bound  = bound_for(cursor, bucket_size);

        const CorrectionOutcome outcome = corrector_.resolve(
            manager_, verifier_, bound, [this, &cursor, bucket_size](CandidateManager& m) {
                return this->refill(m, cursor, bucket_size);
            });

        if (outcome.resolved()) {
            const NodeId  node = outcome.verification.node;
            const KeyType key  = static_cast<KeyType>(outcome.verification.key);
            if (substrate_.remove_from_current_bucket(node, key)) {
                ++metrics_.verified_extractions;
                metrics_.entries_skipped += bucket_size > cursor ? (bucket_size - cursor) : 0;
                return QueueEntry<KeyType>{node, key};
            }
            // Unreachable unless the window and the bucket have diverged. Not
            // silently ignored: falling through to the classical scan below
            // still returns a correct answer, and the counter makes the
            // divergence visible instead of merely slow.
            ++metrics_.window_divergences;
        }

        ++metrics_.fallback_scans;
        return substrate_.pop();
    }

    [[nodiscard]] QueueEntry<KeyType> top() const { return substrate_.top(); }
    [[nodiscard]] bool                empty() const noexcept { return substrate_.empty(); }
    [[nodiscard]] std::size_t         size() const noexcept { return substrate_.size(); }

    void clear() noexcept {
        substrate_.clear();
        metrics_.reset();
    }

    [[nodiscard]] std::uint64_t comparison_count() const noexcept {
        // Verification comparisons count as queue work. Reporting only the
        // substrate's would understate the learned queue against the classical
        // ones by exactly the amount the learned machinery costs, which is the
        // comparison the whole project turns on.
        return substrate_.comparison_count() +
               (verifier_.metrics().comparisons - verifier_comparisons_at_reset_);
    }

    /// Zeroes the per-query comparison count.
    ///
    /// The verification counter is rebased rather than cleared. Dijkstra calls
    /// this at the start of every query, and clearing the verifier's metrics
    /// would take prediction accuracy, correction counts and rank displacement
    /// with it -- destroying the experiment's primary results as a side effect
    /// of resetting a performance counter.
    void reset_comparison_count() noexcept {
        substrate_.reset_comparison_count();
        verifier_comparisons_at_reset_ = verifier_.metrics().comparisons;
    }

    void reserve_nodes(std::size_t node_count) { substrate_.reserve_nodes(node_count); }

    // --- Observation ---------------------------------------------------------

    [[nodiscard]] const LpqMetrics&         metrics() const noexcept { return metrics_; }
    [[nodiscard]] const CandidateMetrics&   candidate_metrics() const noexcept {
        return manager_.metrics();
    }
    [[nodiscard]] const VerificationMetrics& verification_metrics() const noexcept {
        return verifier_.metrics();
    }
    [[nodiscard]] const CorrectionMetrics& correction_metrics() const noexcept {
        return corrector_.metrics();
    }

    void reset_metrics() noexcept {
        metrics_.reset();
        manager_.reset_metrics();
        verifier_.reset_metrics();
        corrector_.reset_metrics();
        verifier_comparisons_at_reset_ = 0;
    }

    [[nodiscard]] Substrate&       substrate() noexcept { return substrate_; }
    [[nodiscard]] const Substrate& substrate() const noexcept { return substrate_; }

private:
    /// A valid lower bound on everything not yet offered.
    ///
    /// Two cases, and the second is what makes expansion terminate usefully:
    ///
    ///   * Part of the bucket is still unexamined, so the bound is the bucket's
    ///     cached minimum -- valid for the unexamined remainder and for every
    ///     later bucket.
    ///
    ///   * The whole bucket has been offered. Everything left is in a LATER
    ///     bucket, so its key is at least base + width. Every offered entry
    ///     lies below that, so acceptance is then guaranteed. Expanding to
    ///     cover the bucket therefore always resolves rather than looping.
    [[nodiscard]] QueueBound bound_for(std::size_t cursor, std::size_t bucket_size) {
        if (cursor >= bucket_size) {
            return QueueBound::at_least(
                static_cast<double>(substrate_.base() + substrate_.width()));
        }
        return QueueBound::at_least(static_cast<double>(substrate_.current_bound()));
    }

    /// Offers more of the current bucket after an expansion.
    ///
    /// Continues from `cursor` and never re-offers, which is the contract
    /// CandidateManager::offer asserts on.
    QueueBound refill(CandidateManager& manager, std::size_t& cursor, std::size_t bucket_size) {
        const auto bucket = substrate_.current_bucket();
        while (cursor < bucket.size() && cursor < manager.target_window()) {
            Candidate c;
            c.node  = bucket[cursor].node;
            c.key   = static_cast<double>(bucket[cursor].key);
            c.score = bucket[cursor].score;
            (void)manager.offer(c);
            ++cursor;
            ++metrics_.entries_examined;
        }
        return bound_for(cursor, bucket_size);
    }

    Substrate          substrate_;
    CandidateManager   manager_;
    VerificationEngine verifier_;
    CorrectionEngine   corrector_;
    const Scorer*      scorer_ = nullptr;
    LpqMetrics         metrics_{};
    std::uint64_t      verifier_comparisons_at_reset_ = 0;
};

}  // namespace lpq

#endif  // LPQ_LPQ_LEARNED_PRIORITY_QUEUE_HPP
