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

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "lpq/common/types.hpp"
#include "lpq/features/feature_vector.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/lpq/bucket_substrate.hpp"
#include "lpq/lpq/candidate_manager.hpp"
#include "lpq/lpq/extraction_observer.hpp"
#include "lpq/metrics/phase_profiler.hpp"
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

/// How an extraction is carried out.
///
/// Both strategies accept an entry only when its key is at or below a valid
/// lower bound on everything still queued, so both are exact. They differ in how
/// much machinery stands between the bucket and that comparison.
enum class ExtractionStrategy : std::uint8_t {
    /// The modular pipeline of ESD.md section 10: copy the front into the
    /// CandidateManager, VerificationEngine, CorrectionEngine, then remove the
    /// winner by searching the bucket. Kept as the reference implementation and
    /// as the only strategy that implements the approximate correctness policy.
    kModular = 0,

    /// Verify in place. Walk the substrate's front in score order and accept the
    /// first entry whose key meets the bound; on a miss, finish the scan of the
    /// bucket (the classical fallback) from where the front ended. No candidate
    /// copy, no sort, no second search to remove. Exact mode only.
    kDirect = 1,
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
                         CorrectnessPolicy policy = CorrectnessPolicy::exact(),
                         ExtractionStrategy strategy = ExtractionStrategy::kModular,
                         std::size_t ring_size = Substrate::kBucketCount)
        : substrate_(bucket_width, ring_size), manager_(window), verifier_(policy), strategy_(strategy),
          front_cap_(window.k_max) {
        if (strategy == ExtractionStrategy::kDirect && policy.slack_factor() != 1.0) {
            throw InvalidConfigError(
                "ExtractionStrategy::kDirect implements exact extraction only; the "
                "approximate correctness policy requires ExtractionStrategy::kModular");
        }
        substrate_.set_front_size(window.k_max);
    }

    [[nodiscard]] ExtractionStrategy strategy() const noexcept { return strategy_; }

    /// Sets the substrate front length (0 disables front maintenance). Only
    /// meaningful for scorers that carry information; a constant scorer gains
    /// nothing from a front and pays for maintaining one.
    void set_front_size(std::size_t k) noexcept {
        substrate_.set_front_size(k);
        front_cap_ = k;
    }

    void set_scorer(const Scorer* scorer) noexcept { scorer_ = scorer; }

    /// Attaches something that watches extractions and may steer them.
    ///
    /// Null by default and left null in every non-adaptive configuration, so
    /// the baseline pays one predictable branch per pop rather than a virtual
    /// call. See extraction_observer.hpp on why this is an interface rather
    /// than a direct dependency on the adaptation layer.
    void set_observer(ExtractionObserver* observer) noexcept { observer_ = observer; }
    [[nodiscard]] const ExtractionObserver* observer() const noexcept { return observer_; }

    // --- PriorityQueueLike ---------------------------------------------------

    void push(NodeId node, KeyType key) {
        LPQ_PROFILE_SCOPE(QueuePhase::kPush);
        substrate_.push(node, key, scorer_ == nullptr ? 0.0F : scorer_->score_of(node));
    }

    [[nodiscard]] QueueEntry<KeyType> pop() {
        LPQ_PROFILE_SCOPE(QueuePhase::kPop);
        ++metrics_.extractions;

        // The observer decides whether the learned path runs at all. A
        // controller that has concluded prediction is not paying for itself
        // retreats here, and the extraction takes the classical bucket scan --
        // which is the fallback guarantee made adaptive rather than a new path.
        const ExtractionPolicy policy =
            observer_ != nullptr ? observer_->policy() : ExtractionPolicy{};

        LPQ_PROFILE_BEGIN(traverse);
        const std::size_t bucket_size = substrate_.current_bucket_size();
        const auto        front       = substrate_.current_front();
        LPQ_PROFILE_END(traverse, QueuePhase::kBucketTraversal);

        if (!policy.use_learned_path) {
            ++metrics_.fallback_scans;
            LPQ_PROFILE_SCOPE(QueuePhase::kFallbackScan);
            const QueueEntry<KeyType> entry = substrate_.pop();
            if (observer_ != nullptr) {
                // Reported so a retreat is visible to the controller as an
                // extraction that skipped nothing, rather than as silence that
                // would let it conclude the retreat had no cost.
                observer_->on_extraction(0, 0, CorrectionOutcome{});
            }
            return entry;
        }

        // The direct strategy needs no front: with a front of zero it is the
        // classical scan plus an early exit on the bound.
        if (strategy_ == ExtractionStrategy::kDirect) {
            return pop_direct(policy, bucket_size);
        }

        if (front.empty()) {
            ++metrics_.fallback_scans;
            LPQ_PROFILE_SCOPE(QueuePhase::kFallbackScan);
            return substrate_.pop();
        }

        // --- Build the candidate window from the front, which cost nothing ---
        LPQ_PROFILE_BEGIN(window);
        manager_.begin_extraction(substrate_.size());
        std::size_t offered = 0;
        // The policy's window is a CAP, not a replacement: the manager's own
        // target already accounts for queue size, and a controller widening
        // beyond it would be asking the window to hold entries the substrate
        // never put in the front.
        const std::size_t window_cap =
            observer_ != nullptr
                ? std::min<std::size_t>(manager_.target_window(), policy.window_size)
                : manager_.target_window();

        for (const auto& e : front) {
            if (offered >= window_cap) {
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
        LPQ_PROFILE_END(window, QueuePhase::kWindowMaintenance);

        // --- Verify, expanding into the rest of the bucket if needed ---------
        //
        // The refill lambda reaches back into the bucket, so its cost lands in
        // the verification phase rather than in bucket traversal. That is the
        // honest attribution: the traversal only happens because verification
        // asked for it, and separating them would need a probe inside a lambda
        // called once per correction round, whose own cost would then be a
        // significant fraction of what it measures.
        LPQ_PROFILE_BEGIN(verify);
        std::size_t      cursor = offered;
        const QueueBound bound  = bound_for(cursor, bucket_size);

        const CorrectionOutcome outcome = corrector_.resolve(
            manager_, verifier_, bound, [this, &cursor, bucket_size](CandidateManager& m) {
                return this->refill(m, cursor, bucket_size);
            });
        LPQ_PROFILE_END(verify, QueuePhase::kVerificationScan);

        if (outcome.resolved()) {
            const NodeId  node = outcome.verification.node;
            const KeyType key  = static_cast<KeyType>(outcome.verification.key);
            LPQ_PROFILE_BEGIN(remove);
            const bool removed = substrate_.remove_from_current_bucket(node, key);
            LPQ_PROFILE_END(remove, QueuePhase::kRemoveFromBucket);
            if (removed) {
                ++metrics_.verified_extractions;
                const std::uint64_t skipped =
                    bucket_size > cursor ? (bucket_size - cursor) : 0;
                metrics_.entries_skipped += skipped;
                if (observer_ != nullptr) {
                    observer_->on_extraction(
                        skipped,
                        static_cast<std::uint32_t>(outcome.verification.comparisons),
                        outcome);
                }
                return QueueEntry<KeyType>{node, key};
            }
            // Unreachable unless the window and the bucket have diverged. Not
            // silently ignored: falling through to the classical scan below
            // still returns a correct answer, and the counter makes the
            // divergence visible instead of merely slow.
            ++metrics_.window_divergences;
        }

        ++metrics_.fallback_scans;
        LPQ_PROFILE_SCOPE(QueuePhase::kFallbackScan);
        const QueueEntry<KeyType> entry = substrate_.pop();
        if (observer_ != nullptr) {
            // Zero entries skipped: a fallback examined all of them. That is
            // the signal the controller uses to retreat, so reporting anything
            // else here would hide the cost of the path being taken.
            observer_->on_extraction(
                0, static_cast<std::uint32_t>(outcome.verification.comparisons), outcome);
        }
        return entry;
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
    /// In-place extraction (ExtractionStrategy::kDirect).
    ///
    /// Correctness argument, which is the same one the modular path makes: the
    /// substrate's bound is a lower bound on every key in the current bucket and
    /// in every later bucket (bucket_substrate.hpp, "The bound goes stale, on
    /// purpose"). An entry whose key is at or below it is therefore a minimum of
    /// the whole queue. Nothing here consults a score for correctness; scores
    /// only choose the ORDER in which entries are looked at, so a bad score costs
    /// comparisons and never a wrong answer.
    ///
    /// Probe the front in score order and stop at the first entry meeting the
    /// bound -- the verification engine has to scan its whole window because it
    /// selects the window minimum, whereas the bound makes any hit a minimum.
    /// If the front misses, the scan simply continues through the rest of the
    /// bucket: that is the classical fallback, entered without restarting. A
    /// completed scan with no early hit has found the exact minimum, so the
    /// bound is refreshed for free (the same amortisation as locate_minimum()).
    [[nodiscard]] QueueEntry<KeyType> pop_direct(const ExtractionPolicy& policy,
                                                 std::size_t             bucket_size) {
        LPQ_PROFILE_BEGIN(verify);
        const std::span<const typename Substrate::Entry> bucket = substrate_.peek_current_bucket();
        const KeyType bound = substrate_.current_bound();

        const std::size_t cap =
            observer_ != nullptr ? std::min<std::size_t>(policy.window_size, front_cap_)
                                 : front_cap_;
        const std::size_t probe = std::min(cap, bucket_size);

        std::size_t best     = 0;
        KeyType     best_key = bucket[0].key;
        bool        hit      = best_key <= bound;
        std::size_t i        = 1;
        // Comparisons: one per entry examined, counting the first.
        std::uint64_t compared = 1;

        for (; !hit && i < probe; ++i) {
            ++compared;
            const auto& e = bucket[i];
            if (e.key < best_key) {
                best = i;
                best_key = e.key;
            }
            hit = best_key <= bound;
        }
        const std::size_t probed = i;

        bool scanned_past_front = false;
        if (!hit) {
            // The front missed. Finish the classical scan from where it ended.
            scanned_past_front = probed < bucket_size;
            for (; i < bucket_size; ++i) {
                ++compared;
                const auto& e = bucket[i];
                if (e.key < best_key) {
                    best = i;
                    best_key = e.key;
                    if (best_key <= bound) {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit) {
                // Full scan, no early exit: best_key is the exact bucket minimum.
                substrate_.set_current_bound(best_key);
            }
        }
        substrate_.add_comparisons(compared);
        LPQ_PROFILE_END(verify, QueuePhase::kVerificationScan);

        const QueueEntry<KeyType> entry = substrate_.take_from_current(best);

        ++metrics_.verified_extractions;
        metrics_.entries_examined += compared;
        const std::uint64_t skipped = bucket_size > compared ? (bucket_size - compared) : 0;
        metrics_.entries_skipped += skipped;

        if (observer_ != nullptr) {
            CorrectionOutcome outcome;
            outcome.path = scanned_past_front ? ResolutionPath::kFallback
                           : (best == 0 ? ResolutionPath::kFirstAttempt
                                        : ResolutionPath::kFirstAttemptCorrected);
            outcome.attempts                     = 1;
            outcome.comparisons                  = static_cast<std::uint32_t>(compared);
            outcome.verification.outcome         = VerificationOutcome::kAccepted;
            outcome.verification.node            = entry.node;
            outcome.verification.key             = static_cast<double>(entry.key);
            outcome.verification.prediction_was_correct = (best == 0);
            outcome.verification.rank_displacement      = static_cast<std::uint32_t>(best);
            outcome.verification.comparisons            = static_cast<std::uint32_t>(compared);
            observer_->on_extraction(scanned_past_front ? 0 : skipped,
                                     static_cast<std::uint32_t>(compared), outcome);
        }
        if (scanned_past_front) {
            ++metrics_.fallback_scans;
        }
        return entry;
    }

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

    /// Null in every non-adaptive configuration. See set_observer().
    ExtractionObserver* observer_ = nullptr;
    ExtractionStrategy  strategy_ = ExtractionStrategy::kModular;
    std::size_t         front_cap_ = 0;
    LpqMetrics         metrics_{};
    std::uint64_t      verifier_comparisons_at_reset_ = 0;
};

}  // namespace lpq

#endif  // LPQ_LPQ_LEARNED_PRIORITY_QUEUE_HPP
